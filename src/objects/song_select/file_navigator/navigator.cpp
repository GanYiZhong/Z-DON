#include "navigator.h"

#include <algorithm>
#include <cctype>

#include "../../../libs/encoding.h"
#include "../../../libs/file/filesystem.h"
#include "../../../libs/fixed_step.h"
#include "../../../libs/gfx/han_fold_table.h"
#include "../../../libs/gfx/text.h"
#include "../../../libs/input/zdon_mouse.h"
#include "../../../libs/jobs.h"
#include "../../../libs/library_index.h"
#include "../../../libs/network/network.h"
#include "../../../libs/notify.h"
#include "../../../libs/zdon_lang.h"
#include "../../dan_select/dan_navigator.h"
#include "../../game/zdon_ai_board.h"
#include "../song_select_script.h"
#include "box_back.h"
#include "box_song_osu.h"
#include "color_utils.h"
#include "navigation_load.h"
#ifdef _WIN32
#include "../../../platform/platform_windows.h"
#endif
#ifdef SUPPORT_FUMEN
#include "../../../libs/optional/gen3.h"
#include "../../../libs/optional/gen4.h"
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <random>
#include <ranges>
#include <sstream>
#include <thread>
#include <utility>

// Song Search normalisation: ASCII is lower-cased, full-width ASCII (U+FF01..FF5E) becomes
// half-width, and simplified / traditional Chinese characters are folded to their Japanese
// shinjitai form (han_fold_table.h), so a query in any of the three scripts matches a title
// written in another. Invalid UTF-8 bytes pass through unchanged.
static uint32_t han_fold(uint32_t cp) {
    size_t lo = 0, hi = HAN_FOLD_TABLE_SIZE;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (HAN_FOLD_TABLE[mid].from < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return (lo < HAN_FOLD_TABLE_SIZE && HAN_FOLD_TABLE[lo].from == cp) ? HAN_FOLD_TABLE[lo].to : cp;
}

// every title and subtitle of a chart, one per line, as the text the search folds
template<class Titles, class Subtitles>
static std::string joined_titles(const Titles& titles, const Subtitles& subtitles) {
    std::string text;
    for (const auto& [lang, t] : titles) {
        text += t + '\n';
    }
    for (const auto& [lang, t] : subtitles) {
        text += t + '\n';
    }
    return text;
}

// the English text of a title/subtitle map, else its first entry
template<class Map>
static const std::string& en_or_first(const Map& m) {
    auto it = m.find("en");
    return it != m.end() ? it->second : m.begin()->second;
}

// how a song is recorded in favorite_songs
static std::string favorite_key(const SongBox& song) {
    return en_or_first(song.parser.metadata.title) + "|" + en_or_first(song.parser.metadata.subtitle);
}

static std::string search_fold(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        int len = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + len > s.size()) {
            out += static_cast<char>(b);
            i++;
            continue;
        }
        uint32_t cp = len == 1 ? b : len == 2 ? (b & 0x1F) : len == 3 ? (b & 0x0F) : (b & 0x07);
        bool ok = true;
        for (int k = 1; k < len; k++) {
            unsigned char cb = static_cast<unsigned char>(s[i + k]);
            if ((cb & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cb & 0x3F);
        }
        if (!ok) {
            out += static_cast<char>(b);
            i++;
            continue;
        }
        i += len;
        if (cp >= 0xFF01 && cp <= 0xFF5E) {
            cp -= 0xFEE0; // full-width ASCII
        } else if (cp == 0x3000) {
            cp = ' '; // ideographic space
        }
        if (cp < 0x80) {
            out += static_cast<char>(std::tolower(static_cast<int>(cp)));
            continue;
        }
        cp = han_fold(cp);
        if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

static std::unique_ptr<SongBox> make_song_box(const fs::path& path, const BoxDef& box_def, SongParser parser) {
    if (path.extension() == ".osu") {
        return std::make_unique<SongBoxOsu>(path, box_def, std::move(parser));
    }
    return std::make_unique<SongBox>(path, box_def, std::move(parser));
}

static std::string u8_key(const fs::path& path) {
    auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

// Dev hook YATAIDON_BOX_RECORD_VERIFY=1: every chart a box is made from a record for is read as well,
// and a record that differs from what the chart gives now is logged as an error.
static bool verify_box_records() {
    static const bool on = [] {
        const char* v = std::getenv("YATAIDON_BOX_RECORD_VERIFY");
        return v && *v && *v != '0';
    }();
    return on;
}

// The parsers of `paths` for their song boxes. A chart with a box record for the version the loading
// screen saw gets a parser made from it (no file is read; SongParser reads the chart once something
// needs more than the metadata), the others are read as before and their records stored for next time.
static std::unordered_map<std::string, std::unique_ptr<SongParser>>
parse_songs_parallel(const std::vector<fs::path>& paths, std::atomic<bool>& abort_flag,
                     jobs::Priority priority = jobs::Priority::Critical, bool verify = verify_box_records()) {
    std::vector<std::unique_ptr<SongParser>> parsed(paths.size());
    std::vector<std::string> keys;
    keys.reserve(paths.size());
    for (const fs::path& p : paths) {
        keys.push_back(u8_key(p));
    }
    std::vector<std::string> records = scores_manager.get_song_box_records(keys);
    size_t from_records = 0;
    for (size_t i = 0; i < paths.size(); i++) {
        if (!records[i].empty()) {
            if (auto p = SongParser::from_box_record(paths[i], records[i])) {
                parsed[i] = std::make_unique<SongParser>(std::move(*p));
                from_records++;
                continue;
            }
        }
        records[i].clear(); // read below; its record is stored
    }
    std::atomic<int> mismatches{0};
    unsigned workers = std::clamp(std::thread::hardware_concurrency(), 2u, 8u);
    if (from_records < paths.size() || verify) {
        jobs::parallel_for(
            paths.size(), priority,
            [&](size_t i) {
                if (abort_flag.load() || (parsed[i] && !verify)) {
                    return;
                }
                try {
                    auto full = std::make_unique<SongParser>(paths[i]);
                    if (parsed[i]) { // verify: the record against the chart, and what was made of it
                        const std::string now = full->box_record();
                        if (now != records[i] || parsed[i]->box_record() != records[i]) {
                            spdlog::error("Box record of {} differs from the chart", paths[i].string());
                            mismatches++;
                        }
                        return;
                    }
                    records[i] = full->box_record();
                    parsed[i] = std::move(full);
                } catch (const std::exception& e) {
                    if (!parsed[i]) {
                        spdlog::warn("Failed to parse {}: {}", paths[i].string(), e.what());
                    } else {
                        spdlog::error("Box record of {}: the chart cannot be read: {}", paths[i].string(), e.what());
                        mismatches++;
                    }
                }
            },
            workers);
        std::vector<std::pair<std::string, std::string>> fresh;
        for (size_t i = 0; i < paths.size(); i++) {
            if (parsed[i] && parsed[i]->parsed() && !records[i].empty()) {
                fresh.emplace_back(std::move(keys[i]), std::move(records[i]));
            }
        }
        scores_manager.store_song_box_records(fresh);
    }
    if (verify) {
        spdlog::info("Song boxes: {} charts, {} from box records, {} records differ", paths.size(), from_records,
                     mismatches.load());
    } else if (!paths.empty()) {
        spdlog::debug("Song boxes: {} charts, {} from box records", paths.size(), from_records);
    }

    std::unordered_map<std::string, std::unique_ptr<SongParser>> out;
    for (size_t i = 0; i < paths.size(); i++) {
        if (parsed[i]) {
            out.emplace(paths[i].string(), std::move(parsed[i]));
        }
    }
    return out;
}

static SongParser take_parser(std::unordered_map<std::string, std::unique_ptr<SongParser>>& preparsed,
                              const fs::path& path) {
    auto it = preparsed.find(path.string());
    if (it != preparsed.end()) {
        SongParser p = std::move(*it->second);
        preparsed.erase(it);
        return p;
    }
    const std::string key = u8_key(path);
    const std::vector<std::string> record = scores_manager.get_song_box_records({key});
    if (!record[0].empty()) {
        if (auto p = SongParser::from_box_record(path, record[0])) {
            return std::move(*p);
        }
    }
    SongParser p(path);
    scores_manager.store_song_box_records({{key, p.box_record()}});
    return p;
}

static std::unique_ptr<BackBox> make_back_box(const fs::path& parent_path, const BoxDef* folder = nullptr) {
    BoxDef d;
    d.back_color = {BackBox::COLOR};
    d.fore_color = BackBox::COLOR;
    d.genre_index = folder ? folder->genre_index : GenreIndex::NAMCO;
    return std::make_unique<BackBox>(parent_path, d);
}

static bool alpha_less(const std::string& a, const std::string& b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = std::tolower(static_cast<unsigned char>(a[i]));
        unsigned char cb = std::tolower(static_cast<unsigned char>(b[i]));
        if (ca != cb) {
            return ca < cb;
        }
    }
    return a.size() < b.size();
}

// Z-DON: sorts boxes by alpha_less of key(box), each key made once (the comparator used to strip markup /
// build a filename string twice per comparison)
template<class KeyFn>
static void sort_boxes_by(std::vector<std::unique_ptr<BaseBox>>& boxes, KeyFn key) {
    std::vector<std::pair<std::string, std::unique_ptr<BaseBox>>> keyed;
    keyed.reserve(boxes.size());
    for (std::unique_ptr<BaseBox>& b : boxes) {
        std::string k = key(*b);
        keyed.emplace_back(std::move(k), std::move(b));
    }
    std::ranges::sort(keyed, [](const auto& a, const auto& b) { return alpha_less(a.first, b.first); });
    for (size_t i = 0; i < boxes.size(); ++i) {
        boxes[i] = std::move(keyed[i].second);
    }
}

static void apply_song_genre(SongBox* song, const BoxDef& box_def) {
    song->song_genre_index = box_def.genre_index;
    song->song_genre_label = box_def.genre_label;
    if (box_def.box_color.empty() && box_def.back_color.empty() && !box_def.fore_color.has_value()) {
        return;
    }
    BoxColors colors = resolve_box_colors(box_def.box_color, box_def.back_color, box_def.fore_color);
    song->fore_color = colors.outline;
    song->text_color = colors.text;
}

namespace ch = std::chrono;

Navigator::Navigator() = default;

Navigator::~Navigator() {
    join_loader();
    if (song_files_thread.joinable()) {
        song_files_thread.join();
    }
}

void Navigator::emit_wheel_event(int id) {
    wheel_event = id;
    ++wheel_event_seq;
    if ((id == WHEEL_EVENT_OPEN_BEGIN || id == WHEEL_EVENT_OPEN_SWAP) && script) {
        script->start_folder_open();
    }

    if (id != WHEEL_EVENT_OPEN_BEGIN && id != WHEEL_EVENT_OPEN_SWAP && script) {
        script->finish_folder_open();
    }

    if (id == WHEEL_EVENT_OPEN_BEGIN || id == WHEEL_EVENT_CLOSE_BEGIN) {
        wheel_leg_ms = get_current_ms();
    }
}

// The swap waits for the arcade's leg timing only for a skin that draws the legs from the wheel
// events; without one the screen sat empty for the rest of that time.
bool Navigator::swap_is_early(int begin_id) const {
    if (!wheel_events_read || wheel_event != begin_id) {
        return false;
    }
    const double duration_scale =
        global_data.config->general.fast_transitions && begin_id == WHEEL_EVENT_OPEN_BEGIN ? 0.2 : 1.0;

    return (get_current_ms() - wheel_leg_ms) < kSwapDelayMs * duration_scale;
}

void Navigator::wait_for_song_files() {
    if (song_files_thread.joinable()) {
        song_files_thread.join();
    }
}

// A back box after every tenth song of an inline list, so the way out stays within reach.
void Navigator::enqueue_inline_back_every_ten(int songs_added, const fs::path& parent) {
    if (songs_added > 0 && songs_added % 10 == 0) {
        enqueue_inline_box(make_back_box(parent, &inline_back_def));
    }
}

// the genre of the box.def above a song, when it has one
void Navigator::apply_genre_of(const fs::path& song_path, SongBox* song) {
    fs::path genre_folder = find_box_def_folder(song_path);
    if (!genre_folder.empty()) {
        apply_song_genre(song, parse_box_def(genre_folder));
    }
}

void Navigator::clear_folder_caches() {
    {
        std::scoped_lock lock(child_folders_mutex);
        def_file_cache.clear();
        child_folders_cache.clear();
    }
    box_def_cache.clear();
}

void Navigator::clear_directory_state() {
    join_loader();
    is_inline = false;
    inline_state.reset();
    pending_inline_path.reset();
    pending_inline_folder = nullptr;
    genre_bg.reset();
    inline_streaming = false;
    items.clear();
    open_index = 0;
    clear_folder_caches();
    is_processing = true;
    loading_complete = false;
}

void Navigator::load_all_roots() {
    clear_directory_state();
    reloading_roots = true;

    auto walk_roots = [this] {
        for (const fs::path& root : root_paths) {
            if (abort_loading) {
                break;
            }
            loading_complete = false;
            load_current_directory_async(root);
        }
        reloading_roots = false;
        loading_complete = true;
        FolderBox::run_deferred_scans(abort_loading);
        for (const fs::path& root : root_paths) {
            prefetch_child_folders(root);
        }
        dump_library_if_requested();
        selftest_box_records_if_requested();
    };

#ifndef __EMSCRIPTEN__
    loader_thread = std::thread(walk_roots);
    fixed_step::join(loader_thread);
#else
    walk_roots();
#endif
}

// Dev hook: YATAIDON_BOX_RECORD_SELFTEST=1 checks, once, the box record of every chart the loading
// screen indexed against the chart (storing the missing records, so a second run checks them all).
void Navigator::selftest_box_records_if_requested() {
    static std::atomic<bool> done{false};
    const char* on = std::getenv("YATAIDON_BOX_RECORD_SELFTEST");
    if (!on || !*on || *on == '0' || abort_loading || done.exchange(true)) {
        return;
    }
    wait_for_song_files();
    std::vector<fs::path> paths;
    paths.reserve(song_levels.size());
    for (const auto& [path, levels] : song_levels) {
        paths.emplace_back(path);
    }
    std::ranges::sort(paths);
    const size_t had = [&] {
        std::vector<std::string> keys;
        keys.reserve(paths.size());
        for (const fs::path& p : paths) {
            keys.push_back(u8_key(p));
        }
        size_t n = 0;
        for (const std::string& r : scores_manager.get_song_box_records(keys)) {
            n += !r.empty();
        }
        return n;
    }();
    auto parsed = parse_songs_parallel(paths, abort_loading, jobs::Priority::Normal, true);
    spdlog::info("Box record selftest: {} charts, {} had a record, {} parsers", paths.size(), had, parsed.size());
}

// Dev hook: YATAIDON_LIBRARY_DUMP=<file> writes, once, after the first song select load and its
// deferred folder scans (loader thread), what the navigator knows about the library: the folder
// scans (tja_count, crowns), every song it indexed, favorites, has_def_file / is_osu_song_folder /
// has_child_folders of every folder below the roots, the folder scan and inline-open plan of every
// folder with a box.def or song_list.txt, and the dojo courses of each DAN folder at the root. Diff
// the files of two builds to check that a change to library scanning keeps the same results.
void Navigator::dump_library_if_requested() {
    static std::atomic<bool> done{false};
    const char* out_path = std::getenv("YATAIDON_LIBRARY_DUMP");
    if (!out_path || !*out_path || abort_loading || done.exchange(true)) {
        return;
    }
    wait_for_song_files();
    std::ostringstream out;

    std::vector<fs::path> dirs;
    for (const fs::path& root : root_paths) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::end(it); it.increment(ec)) {
            std::error_code dec;
            if (it->is_directory(dec)) {
                dirs.push_back(it->path());
            }
        }
    }
    std::ranges::sort(dirs);
    std::vector<fs::path> folder_dirs;
    for (const fs::path& d : dirs) {
        std::error_code ec;
        if (fs::exists(d / "box.def", ec) || fs::exists(d / "song_list.txt", ec)) {
            folder_dirs.push_back(d);
        }
    }

    // Charts sharing a title and subtitle: song_files keeps whichever the preload pool stored last,
    // so which of them a title lookup returns differs from run to run -- shown by their title
    std::map<std::pair<std::string, std::string>, int> title_count;
    for (const auto& [path, lv] : song_levels) {
        if (auto meta = scores_manager.get_song_meta(path)) {
            title_count[{meta->title, meta->subtitle}]++;
        }
    }
    auto show = [&](const fs::path& p) {
        auto u8 = p.u8string();
        if (auto meta = scores_manager.get_song_meta(std::string(u8.begin(), u8.end()))) {
            if (title_count[{meta->title, meta->subtitle}] > 1) {
                return "(one of the charts titled " + meta->title + '|' + meta->subtitle + ')';
            }
        }
        return p.string();
    };

    out << "[dan courses]\n";
    for (const fs::path& root : root_paths) {
        std::error_code ec;
        std::vector<fs::path> dan_dirs;
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (fs::exists(e.path() / "box.def", ec) &&
                parse_box_def_uncached(e.path()).genre_index == GenreIndex::DAN) {
                dan_dirs.push_back(e.path());
            }
        }
        std::ranges::sort(dan_dirs);
        for (const fs::path& dan_dir : dan_dirs) {
            DanNavigator dan;
            for (const DanBoxData& d : dan.scan_for_dump({dan_dir})) {
                out << d.json_path.string() << " title=" << d.title << " rank=" << d.rank << " notes=" << d.total_notes
                    << " songs=";
                for (const DanSongEntry& s : d.songs) {
                    out << show(s.song_path) << '#' << s.difficulty << " level=" << s.level
                        << " genre=" << s.genre_index << ';';
                }
                for (const auto& [t, sub] : d.song_titles) {
                    out << " [" << t << '|' << sub << ']';
                }
                out << '\n';
            }
        }
    }

    out << "[song_files]\n";
    for (const auto& [key, path] : song_files) {
        out << key.first << '|' << key.second << " -> " << show(path) << '\n';
    }
    out << "[song_levels]\n";
    std::vector<std::pair<std::string, std::array<int, 5>>> levels(song_levels.begin(), song_levels.end());
    std::ranges::sort(levels);
    for (const auto& [path, lv] : levels) {
        out << path << ' ' << lv[0] << ',' << lv[1] << ',' << lv[2] << ',' << lv[3] << ',' << lv[4] << '\n';
    }
    out << "[favorites]\n";
    out << "recent=" << (recent_folder_path ? recent_folder_path->string() : "") << '\n';
    out << "favorite=" << (favorite_folder_path ? favorite_folder_path->string() : "") << '\n';
    for (const std::string& f : favorite_songs) {
        out << f << '\n';
    }

    out << "[folders: has_def_file is_osu_song_folder has_child_folders]\n";
    for (const fs::path& d : dirs) {
        out << d.string() << ' ' << has_def_file(d) << ' ' << is_osu_song_folder(d) << ' ';
        try {
            out << has_child_folders(d);
        } catch (const std::exception& e) {
            out << "throws";
        }
        out << '\n';
    }

    FolderBox::dump_scans(out, folder_dirs);

    out << "[inline-open plans]\n";
    for (const fs::path& d : folder_dirs) {
        std::vector<fs::path> song_paths;
        std::unordered_map<std::string, std::vector<std::pair<bool, fs::path>>> plan;
        try {
            scan_song_tree(d, song_paths, plan);
        } catch (const std::exception& e) {
            out << d.string() << " throws " << e.what() << '\n';
            continue;
        }
        out << d.string() << " songs:\n";
        for (const fs::path& p : song_paths) {
            out << "  " << p.string() << '\n';
        }
        std::vector<std::string> keys;
        keys.reserve(plan.size());
        for (const auto& [key, entries] : plan) {
            keys.push_back(key);
        }
        std::ranges::sort(keys);
        for (const std::string& key : keys) {
            out << " plan " << key << ":\n";
            for (const auto& [is_osu, p] : plan[key]) {
                out << "  " << (is_osu ? "osu " : "") << p.string() << '\n';
            }
        }
    }

    if (abort_loading) {
        out << "ABORTED (the navigator was reloaded during the dump: some sections are incomplete)\n";
    }
    std::ofstream(out_path, std::ios::binary) << out.str();
    spdlog::info("YATAIDON_LIBRARY_DUMP: wrote {} ({} folders)", out_path, dirs.size());
}

// Empty when unset or not a box folder inside a song root: the back boxes walk parent paths up to
// an exact root, so the result has to be built on the root's own spelling.
fs::path Navigator::resolve_start_path() {
    if (start_path.empty()) {
        return {};
    }
    for (const fs::path& root : root_paths) {
        fs::path rel = (root / start_path).lexically_normal().lexically_relative(root.lexically_normal());
        if (rel.filename().empty()) {
            rel = rel.parent_path();
        }
        if (rel.empty() || rel == "." || *rel.begin() == "..") {
            continue;
        }
        std::error_code ec;
        if (fs::is_directory(root / rel, ec) && has_def_file(root / rel)) {
            return root / rel;
        }
    }
    spdlog::warn("start_path {} is not a box folder inside tja_path, opening at the root", start_path.string());
    return {};
}

void Navigator::load_start_folder() {
    const fs::path start = resolve_start_path();
    const bool leaf = !start.empty() && !has_child_folders(start);
    const fs::path target = leaf ? start.parent_path() : start;
    start_folder = leaf ? fs::path{} : start;
    // A folder without subfolders only opens inline, from its box in the parent listing.
    if (leaf) {
        restore_cursor_path = start;
        open_start_folder = start;
    }
    if (target.empty() || std::ranges::contains(root_paths, target)) {
        load_all_roots();
        return;
    }
    clear_directory_state();
    load_current_directory(target);
}

void Navigator::reset_for_skin_reload() {
    join_loader();
    wait_for_song_files();

    inline_state.reset();
    pending_inline_folder = nullptr;
    pending_inline_path.reset();
    genre_bg.reset();
    items.clear();
    while (!pending_boxes.empty()) {
        pending_boxes.pop();
    }
    while (!pending_inline_boxes.empty()) {
        pending_inline_boxes.pop();
    }

    script = nullptr;
    is_inline = false;
    inline_streaming = false;
    wheel_events_read = false;
    is_init = false;
}

void Navigator::preload(std::vector<fs::path> songs_paths, std::optional<std::vector<fs::path>> listed,
                        std::vector<SongFileStamp> stamps) {
    if (is_preloaded) {
        return;
    }

    for (fs::path& root : songs_paths) {
        if (root.filename().empty()) {
            root = root.parent_path();
        }
    }

    root_paths = songs_paths;
    open_index = 0;

#ifndef __EMSCRIPTEN__
    song_files_ready = false;
    song_files_thread = std::thread([this, songs_paths, listed = std::move(listed), stamps = std::move(stamps)] {
        std::vector<fs::path> files = listed ? *listed : get_song_files(songs_paths);
        std::mutex map_mutex;
        // YATAIDON_FIXED_STEP_MS: charts sharing a title and subtitle (two audio versions of a song) go to the
        // later one in the listing, as a sequential scan binds them, not to whichever worker wrote last
        std::map<std::pair<std::string, std::string>, size_t> title_owner;
        auto bind_title = [&](std::pair<std::string, std::string> key, const fs::path& file, size_t i) {
            // Z-DON: in every mode (it used to be whichever worker wrote last, so e.g. a dan course
            // played the old or the new audio version of a same-titled song from run to run)
            auto [it, fresh] = title_owner.try_emplace(key, i);
            if (!fresh) {
                if (it->second > i) {
                    return;
                }
                it->second = i;
            }
            song_files[std::move(key)] = file;
        };
        unsigned pool_size = std::max(2u, std::thread::hardware_concurrency() / 2);
        jobs::parallel_for(
            files.size(), jobs::Priority::Background,
            [&](size_t i) {
                {
                    if (abort_loading) {
                        return;
                    }
                    const fs::path& file = files[i];
                    // the listing already says which of these are files, except for a link
                    if (i < stamps.size() && stamps[i].listed && !stamps[i].is_reparse) {
                        const fs::path ext = file.extension();
                        if (stamps[i].is_dir || (ext != ".tja" && ext != ".osu")) {
                            return;
                        }
                    } else if (!is_song_file(file)) {
                        return;
                    }
                    // the loading screen already read this chart (parsed or from its cache)
                    auto u8 = file.u8string();
                    if (auto meta = scores_manager.get_song_meta(std::string(u8.begin(), u8.end()))) {
                        const bool playable =
                            std::any_of(meta->levels.begin(), meta->levels.end(), [](int lv) { return lv >= 0; });
                        if (playable) {
                            std::scoped_lock lock(map_mutex);
                            bind_title({meta->title, meta->subtitle}, file, i);
                            song_search_text[file.string()] = search_fold(joined_titles(meta->titles, meta->subtitles));
                            song_levels[file.string()] = meta->levels;
                        }
                        return;
                    }
                    try {
                        SongParser parsed_entry = SongParser(file);
                        parsed_entry.get_metadata();
                        const bool playable =
                            std::ranges::any_of(parsed_entry.metadata.course_data,
                                                [](const auto& entry) { return entry.first >= 0 && entry.first <= 4; });
                        if (playable) {
                            std::scoped_lock lock(map_mutex);
                            bind_title({parsed_entry.metadata.title["en"], parsed_entry.metadata.subtitle["en"]}, file,
                                       i);
                            song_search_text[file.string()] =
                                search_fold(joined_titles(parsed_entry.metadata.title, parsed_entry.metadata.subtitle));
                            std::array<int, 5> lv;
                            lv.fill(-1);
                            for (const auto& [course, data] : parsed_entry.metadata.course_data) {
                                if (course >= 0 && course <= 4) {
                                    lv[course] = static_cast<int>(data.level);
                                }
                            }
                            song_levels[file.string()] = lv;
                        }
                    } catch (const std::exception& inner) {
                        spdlog::warn("Skipping song during scan: {}", inner.what());
                    }
                }
            },
            pool_size);
        song_files_ready = true;
    });
    fixed_step::join(song_files_thread);
#endif // !__EMSCRIPTEN__

    for (const fs::path& root_path : songs_paths) {
        for (const auto& entry : fs::directory_iterator(root_path)) {
            if (!fs::is_directory(entry) || !has_def_file(entry.path())) {
                continue;
            }
            BoxDef bd = parse_box_def(entry.path());
            if (bd.collection == "RECENT" && !recent_folder_path) {
                recent_folder_path = entry.path();
            }
            if (bd.collection == "FAVORITE" && !favorite_folder_path) {
                favorite_folder_path = entry.path();
            }
        }
    }
    set_song_list_folders(favorite_folder_path.value_or(fs::path()), recent_folder_path.value_or(fs::path()));
    reload_favorites();
    is_preloaded = true;
}

void Navigator::reload_favorites() {
    favorite_songs.clear();
    if (!favorite_folder_path) {
        return;
    }
    for (const auto& e : read_song_list(song_list_file(*favorite_folder_path))) {
        favorite_songs.insert(e.title + "|" + e.subtitle);
    }
}

void Navigator::init(std::vector<fs::path> songs_paths) {
    t_background = tex.get_texture("box/background");
    emit_wheel_event(WHEEL_EVENT_SCENE_ENTRY);
    if (is_init && (hide_dan != built_hide_dan || hide_collections != built_hide_collections ||
                    waiwai != built_waiwai || is_2p != built_is_2p || ai_assigned != built_ai_assigned)) {
        join_loader();
        is_inline = false;
        inline_state.reset();
        pending_inline_path.reset();
        pending_inline_folder = nullptr;
        genre_bg.reset();
        awaiting_diff_sort = false;
        diff_sort_filter.reset();
        reopen_folder_path.reset();
        reopen_song_path.reset();
        items.clear();
        open_index = 0;
        is_init = false;
    }
    built_hide_dan = hide_dan;
    built_hide_collections = hide_collections;
    built_waiwai = waiwai;
    built_ai_assigned = ai_assigned;
    built_is_2p = is_2p;

    if (!is_init) {
        if (!is_preloaded) {
            preload(std::move(songs_paths));
        }
        reload_favorites();

        load_start_folder();
        is_init = true;
    } else {
        if (global_data.config->limits.song_limit > 0) {
            join_loader();
            is_inline = false;
            inline_state.reset();
            pending_inline_path.reset();
            pending_inline_folder = nullptr;
            genre_bg.reset();
            awaiting_diff_sort = false;
            diff_sort_filter.reset();
            load_start_folder();
        } else {
            if (inline_state.has_value()) {
                if (open_index >= 0 && open_index < (int)items.size()) {
                    if (dynamic_cast<SongBox*>(items[open_index].get())) {
                        reopen_song_path = items[open_index]->path;
                    }
                }
                reopen_folder_path = inline_state->saved_folder_box
                                         ? std::optional<fs::path>(inline_state->saved_folder_box->path)
                                         : std::nullopt;
                if (!reopen_folder_path) {
                    reopen_song_path.reset();
                }
                collapse_inline_now();
            }
            for (auto& item : items) {
                item->reset();
                item->fade_in(0);
            }
            if (pending_inline_folder != nullptr) {
                pending_inline_folder->reset();
                pending_inline_folder->expand_box();
            }
            set_positions(false, 0);
            if (!items.empty()) {
                get_current_item()->expand_box();
            }
        }
    }
    vertical_gallery = tex.options.contains(SCO::HORIZONTAL_SONG_SELECT) && tex.options.at(SCO::HORIZONTAL_SONG_SELECT);
    background_move = static_cast<MoveAnimation*>(tex.get_animation(0));
    background_fade_change = static_cast<FadeAnimation*>(tex.get_animation(5));
    bg_genre_index = items.empty() ? GenreIndex::TUTORIAL : items[open_index]->genre_index;
    last_bg_genre_index = bg_genre_index;
    bg_genre_pending = true;
    if (genre_bg.has_value()) {
        genre_bg->fade_in();
    }
}

void Navigator::collapse_inline_now() {
    if (!inline_state.has_value()) {
        return;
    }
    join_loader();

    InlineState& state = *inline_state;
    int end = state.first_song_index + state.songs_count;
    if (state.first_song_index < 0 || end > static_cast<int>(items.size()) || state.folder_index < 0 ||
        state.folder_index >= static_cast<int>(items.size()) || !state.saved_folder_box) {
        return;
    }

    items.erase(items.begin() + state.first_song_index, items.begin() + end);
    items.erase(items.begin() + state.folder_index);
    items.insert(items.begin() + state.folder_index, std::move(state.saved_folder_box));
    open_index = state.folder_index;
    items[open_index]->exit_box();

    {
        std::scoped_lock lock(pending_mutex);
        std::queue<std::unique_ptr<BaseBox>>().swap(pending_inline_boxes);
    }

    inline_state.reset();
    pending_inline_folder = nullptr;
    pending_inline_path.reset();
    genre_bg.reset();
    is_inline = false;
    is_processing = false;
    inline_streaming = false;
    loading_complete = false;
}

void Navigator::join_loader() {
    abort_loading = true;
    if (loader_thread.joinable()) {
        loader_thread.join();
    }
    join_prefetch();
    abort_loading = false;
}

void Navigator::enqueue_box(std::unique_ptr<BaseBox> box) {
    std::error_code ec;
    auto last_write = fs::last_write_time(box->path, ec);
    if (!ec) {
        auto last_write_sys = ch::system_clock::now() + std::chrono::duration_cast<ch::system_clock::duration>(
                                                            last_write - std::filesystem::file_time_type::clock::now());
        auto two_weeks_ago = ch::system_clock::now() - ch::weeks(2);
        if (last_write_sys < two_weeks_ago) {
            box->is_new = true;
        }
    }

    box->preregister_text(); // glyphs for the whole batch land before the first load_text()

    std::scoped_lock lock(pending_mutex);
    if (auto* song = dynamic_cast<SongBox*>(box.get())) {
        if (favorite_songs.contains(favorite_key(*song))) {
            song->is_favorite = true;
        }
    }
    pending_boxes.push(std::move(box));
}

void Navigator::enqueue_inline_box(std::unique_ptr<BaseBox> box) {
    std::scoped_lock lock(pending_mutex);
    if (auto* song = dynamic_cast<SongBox*>(box.get())) {
        if (favorite_songs.contains(favorite_key(*song))) {
            song->is_favorite = true;
        }
    }
    pending_inline_boxes.push(std::move(box));
}

void sort_items(std::vector<std::unique_ptr<BaseBox>>& items, int first_index, int last_index) {
    if (first_index >= 0 && last_index > first_index && last_index < static_cast<int>(items.size())) {

        std::vector<int> back_box_positions;
        std::vector<std::unique_ptr<BaseBox>> sortable;

        for (int i = first_index; i <= last_index; i++) {
            if (items[i]->text_name == "BACK_BOX" || items[i]->preserve_order) {
                back_box_positions.push_back(i - first_index);
            } else {
                sortable.push_back(std::move(items[i]));
            }
        }

        sort_boxes_by(sortable, [](const BaseBox& b) { return strip_text_markup(b.text_name); });

        int sortable_idx = 0;
        for (int i = first_index; i <= last_index; i++) {
            int relative = i - first_index;
            if (std::ranges::contains(back_box_positions, relative)) {
                // BACK_BOX slot — item was never moved, nothing to do
            } else {
                items[i] = std::move(sortable[sortable_idx++]);
            }
        }
    }
}

void Navigator::refresh_scores() {
    FolderBox::invalidate_scan_cache();
    SongBox* curr_item = dynamic_cast<SongBox*>(get_current_item());
    if (curr_item) {
        curr_item->refresh_scores();
    }
    if (pending_inline_folder) {
        pending_inline_folder->refresh_scores();
    }
}

void Navigator::flush_pending_boxes() {
    std::scoped_lock lock(pending_mutex);

    while (!pending_boxes.empty()) {
        items.push_back(std::move(pending_boxes.front()));
        pending_boxes.pop();
    }

    if (inline_state.has_value()) {
        if (!pending_inline_boxes.empty()) {
            std::vector<std::unique_ptr<BaseBox>> batch;
            while (!pending_inline_boxes.empty()) {
                batch.push_back(std::move(pending_inline_boxes.front()));
                pending_inline_boxes.pop();
            }
            int insert_pos = inline_state->first_song_index + inline_state->songs_count;
            items.insert(items.begin() + insert_pos, std::make_move_iterator(batch.begin()),
                         std::make_move_iterator(batch.end()));
            inline_state->songs_count += static_cast<int>(batch.size());
        }
        genre_bg_start = std::max(0, inline_state->first_song_index - 1);
        genre_bg_end = inline_state->first_song_index + inline_state->songs_count - 1;
    }

    if (loading_complete.load() && inline_state.has_value() && swap_is_early(WHEEL_EVENT_OPEN_BEGIN)) {
        return;
    }

    if (loading_complete.load()) {
        if (open_index >= items.size()) {
            open_index = 0;
        }
        if (inline_state.has_value()) {
            sort_items(items, genre_bg_start, genre_bg_end);
        } else if (items.size() > 1) {
            std::vector<int> sortable_indices;
            std::vector<std::unique_ptr<BaseBox>> sortable;
            int first_sortable = dynamic_cast<BackBox*>(items[0].get()) ? 1 : 0;
            for (int i = first_sortable; i < static_cast<int>(items.size()); i++) {
                if (!items[i]->preserve_order) {
                    sortable_indices.push_back(i);
                    sortable.push_back(std::move(items[i]));
                }
            }
            sort_boxes_by(sortable, [](const BaseBox& b) { return b.path.filename().string(); });
            for (int j = 0; j < static_cast<int>(sortable_indices.size()); j++) {
                items[sortable_indices[j]] = std::move(sortable[j]);
            }
        }
        if (restore_cursor_path && !inline_state.has_value()) {
            for (int i = 0; i < static_cast<int>(items.size()); i++) {
                if (items[i]->path == *restore_cursor_path) {
                    open_index = i;
                    break;
                }
            }
            restore_cursor_path.reset();
        }

        if (inline_state.has_value() && reopen_song_path && reopen_folder_path) {
            const auto& folder = inline_state->saved_folder_box;
            if (folder && folder->path == *reopen_folder_path) {
                int first = inline_state->first_song_index;
                int end = std::min(first + inline_state->songs_count, static_cast<int>(items.size()));
                for (int i = first; i < end; i++) {
                    if (items[i]->path == *reopen_song_path) {
                        open_index = i;
                        break;
                    }
                }
            }
            reopen_folder_path.reset();
            reopen_song_path.reset();
        }

        set_positions(false, 800);
        if (!items.empty()) {
            items[open_index]->expand_box();
        }
        is_processing = false;
        if (inline_streaming && inline_state.has_value()) {
            emit_wheel_event(WHEEL_EVENT_OPEN_SWAP);
        }
        inline_streaming = false;
        loading_complete = false;
    }
}

void Navigator::parse_song_list(const fs::path& path, const BoxDef& box_def, bool inline_mode) {
    auto entries = read_song_list(path);
    std::vector<SongListEntry> out_entries;
    bool needs_rewrite = false;
    int songs_added = 0;

    // Resolve every entry first so the songs can be parsed in parallel
    std::vector<fs::path> resolved(entries.size());
    for (size_t i = 0; i < entries.size(); i++) {
        const auto& entry = entries[i];
        fs::path& final_path = resolved[i];

        // Hash is the primary reference. If several songs share it, title and
        // subtitle disambiguate only among candidates with that same hash.
        if (auto found = scores_manager.get_path_by_hash(entry.hash)) {
            final_path = *found;
        }
        if (!entry.title.empty() || !entry.subtitle.empty()) {
            if (auto by_title = find_song_by_title(entry.title, entry.subtitle);
                by_title && scores_manager.get_single_hash(*by_title) == entry.hash) {
                final_path = *by_title;
            }
        }
        if (final_path.empty()) {
            if (auto song_path_opt = find_song_by_title(entry.title, entry.subtitle)) {
                final_path = *song_path_opt;
            }
        }
    }
    std::vector<fs::path> to_parse;
    for (const fs::path& p : resolved) {
        if (!p.empty()) {
            to_parse.push_back(p);
        }
    }
    auto preparsed = parse_songs_parallel(to_parse, abort_loading);

    for (size_t i = 0; i < entries.size(); i++) {
        const auto& entry = entries[i];
        const fs::path& final_path = resolved[i];
        if (final_path.empty()) {
            out_entries.push_back(entry);
            spdlog::warn("No song found for: {} | {}", entry.title, entry.subtitle);
            continue;
        }

        SongParser parser = take_parser(preparsed, final_path);
        const auto& titles = parser.metadata.title;
        const auto& subtitles = parser.metadata.subtitle;
        const std::string title = titles.contains("en") ? titles.at("en") : titles.begin()->second;
        const std::string subtitle = subtitles.contains("en") ? subtitles.at("en") : subtitles.begin()->second;
        const std::string correct_hash = scores_manager.get_single_hash(final_path);
        out_entries.push_back({correct_hash, title, subtitle});
        needs_rewrite |= entry.hash != correct_hash || entry.title != title || entry.subtitle != subtitle;

        auto box = make_song_box(final_path, box_def, std::move(parser));
        box->preserve_order = true;
        enqueue_inline_back_every_ten(songs_added, path.parent_path().parent_path());
        if (inline_mode) {
            enqueue_inline_box(std::move(box));
        } else {
            enqueue_box(std::move(box));
        }
        songs_added++;
    }

    if (needs_rewrite) {
        queue_song_list_write(path, out_entries);
    }
}

void Navigator::load_current_directory_async(const fs::path path) {
    BoxDef box_def = parse_box_def(path);

    setup_back_box(path, true);

#ifdef SUPPORT_FUMEN
    fs::path own_root = gen4::find_data_root(path);
    bool own_root_is_gen3 = false;
    if (own_root.empty() || !gen4::library_for(own_root)) {
        fs::path gen3_root = gen3::find_data_root(path);
        if (!gen3_root.empty() && gen3::library_for(gen3_root)) {
            own_root = gen3_root;
            own_root_is_gen3 = true;
        }
    }
    bool is_a_root = std::find(root_paths.begin(), root_paths.end(), path) != root_paths.end();
    if (!own_root.empty() && is_a_root && (own_root_is_gen3 || gen4::library_for(own_root))) {
        if (reloading_roots && !only_gen4_songs()) {
            BoxDef gen4_def = box_def;
            gen4_def.name = own_root.filename().string();
            gen4_def.genre_index = GenreIndex::DEFAULT;
            enqueue_box(std::make_unique<FolderBox>(own_root, gen4_def));
            if (!reloading_roots) {
                loading_complete = true;
            }
            current_path = path;
            return;
        }
        for (const fs::path& root : root_paths) {
            if (!gen4::find_data_root(root).empty()) {
                continue;
            }
            items.push_back(make_back_box(root));
            break;
        }
        try {
            if (own_root_is_gen3) {
                load_gen3_genres(own_root);
            } else {
                load_gen4_genres(own_root);
            }
        } catch (const std::exception& e) {
            spdlog::error("Error listing arcade genres of {}: {}", own_root.string(), e.what());
        }
        if (!reloading_roots) {
            loading_complete = true;
        }
        current_path = path;
        return;
    }
#endif

    const bool gen3_root = is_gen3_root(path);
    if (gen3_root && path.filename() == "data" && path.parent_path().filename() == "USRDIR" && !items.empty()) {
        if (auto* back = dynamic_cast<BackBox*>(items.front().get())) {
            back->path = path.parent_path().parent_path().parent_path();
        }
    }

    if (gen3_root || is_gen4_root(path)) {
        try {
            if (gen3_root) {
                load_gen3_genres(path);
            } else {
                load_gen4_genres(path);
            }
        } catch (const std::exception& e) {
            spdlog::error("Error listing gen4 genres of {}: {}", path.string(), e.what());
        } catch (...) {
            spdlog::error("Unknown error listing gen4 genres of {}", path.string());
        }
        if (!reloading_roots) {
            loading_complete = true;
        }
        current_path = path;
        return;
    }

    std::vector<fs::path> song_paths;
    try {
        for (const fs::directory_entry& entry : fs::directory_iterator(path)) {
            if (abort_loading) {
                break;
            }
            if (!fs::is_directory(entry.path()) && is_song_file(entry.path())) {
                song_paths.push_back(entry.path());
            }
        }
    } catch (const fs::filesystem_error&) { /* main loop reports errors */
    }
    auto preparsed = parse_songs_parallel(song_paths, abort_loading);

    try {
        for (const fs::directory_entry& entry : fs::directory_iterator(path)) {
            if (abort_loading) {
                break;
            }
            const fs::path& curr_path = entry.path();
            try {
                if (!fs::is_directory(curr_path)) {
                    if (is_song_list_file(curr_path)) { // upstream #381: an account's own list in favorites / recent
                        wait_for_song_files();
                        BoxDef entry_box_def = parse_box_def(curr_path);
                        parse_song_list(curr_path, entry_box_def, false);
                        continue;
                    }
                    if (is_song_file(curr_path)) {
                        enqueue_box(make_song_box(curr_path, box_def, take_parser(preparsed, curr_path)));
                    }
                    continue;
                }
                fs::path groot = gen3_root_at(curr_path);
                if (is_gen4_root(curr_path) || !groot.empty()) {
                    if (only_gen4_songs()) {
                        if (!groot.empty()) {
                            load_gen3_genres(groot);
                        } else {
                            load_gen4_genres(curr_path);
                        }
                        continue;
                    }
                    BoxDef gen4_def = box_def;
                    gen4_def.name = curr_path.filename().string();
#ifdef SUPPORT_FUMEN
                    if (!groot.empty()) {
                        if (const gen3::Library* lib = gen3::library_for(groot)) {
                            gen4_def.name = lib->game_name();
                        }
                    }
#endif
                    gen4_def.genre_index = GenreIndex::DEFAULT;
                    enqueue_box(std::make_unique<FolderBox>(groot.empty() ? curr_path : groot, gen4_def));
                    continue;
                }
                if (is_gen3_song_folder(curr_path)) {
                    enqueue_box(make_song_box(curr_path, box_def, SongParser(curr_path)));
                    continue;
                }
                if (is_gen4_song_folder(curr_path)) {
                    enqueue_box(make_song_box(curr_path, box_def, SongParser(curr_path)));
                    continue;
                }
                if (has_def_file(curr_path)) {
                    BoxDef entry_box_def = parse_box_def(curr_path);
                    if (hide_dan && entry_box_def.genre_index == GenreIndex::DAN) {
                        continue;
                    }
                    if (entry_box_def.genre_index == GenreIndex::TUTORIAL && !fs::exists(curr_path / TUTORIAL_CHART)) {
                        continue; // the tutorial folder only shows while its chart is there
                    }
                    auto folder = std::make_unique<FolderBox>(curr_path, entry_box_def);
                    if (entry_box_def.collection == "RECOMMENDED") {
                        folder->tja_count = 10;
                    }
                    enqueue_box(std::move(folder));
                } else if (is_osu_song_folder(curr_path)) {
                    BoxDef osu_box_def = box_def;
                    auto it = fs::directory_iterator(curr_path);
                    auto end = fs::end(it);
                    while (it != end && it->path().extension() != ".osu") {
                        ++it;
                    }
                    if (it == end) {
                        continue;
                    }
                    OsuParser title_parser = OsuParser(it->path());
                    title_parser.get_metadata();
                    osu_box_def.name = title_parser.metadata.title[global_data.config->general.language];
                    std::unique_ptr<FolderBox> osu_folder = std::make_unique<FolderBox>(curr_path, osu_box_def);
                    osu_folder->is_osu_folder = true;
                    enqueue_box(std::move(osu_folder));
                } else {
                    std::error_code ec;
                    auto it = fs::recursive_directory_iterator(curr_path, ec);
                    while (it != fs::end(it)) {
                        if (abort_loading) {
                            break;
                        }
                        try {
                            if (fs::is_directory(it->path()) && is_osu_song_folder(it->path())) {
                                it.disable_recursion_pending();
                                BoxDef osu_box_def = box_def;
                                osu_box_def.name = it->path().filename().string();
                                enqueue_box(std::make_unique<FolderBox>(it->path(), osu_box_def));
                            } else if (is_song_file(it->path())) {
                                enqueue_box(make_song_box(it->path(), box_def, SongParser(it->path())));
                            }
                        } catch (const std::exception& inner) {
                            spdlog::warn("Skipping song: {}", inner.what());
                        }
                        it.increment(ec);
                        if (ec) {
                            ec.clear();
                        }
                    }
                }
            } catch (const std::exception& e) {
                spdlog::warn("Skipping entry: {}", e.what());
            }
        }
    } catch (const fs::filesystem_error& e) {
        spdlog::error("Error loading directory: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::error("Error loading directory {}: {}", path.string(), e.what());
    } catch (...) {
        spdlog::error("Unknown error loading directory {}", path.string());
    }
    if (!reloading_roots) {
        loading_complete = true;
    }
    current_path = path;
    if (!reloading_roots) {
        FolderBox::run_deferred_scans(abort_loading);
        prefetch_child_folders(path);
    }
}

// New songs: charts whose folder changed in the last two weeks. The startup scan already listed
// every playable chart (song_levels), so only those folders' write times are read -- no second
// walk of the whole library. Songs live under the siblings of the collection folder. Call only
// once the scan is done (song_levels is read without a lock).
std::vector<fs::path> Navigator::find_new_songs(const fs::path& path) const {
    std::vector<fs::path> hits;
    const fs::path parent = path.parent_path();
    const auto two_weeks_ago = fs::file_time_type::clock::now() - ch::weeks(2);
    std::vector<std::string> index;
    index.reserve(song_levels.size());
    for (const auto& [path_str, levels] : song_levels) {
        index.push_back(path_str);
    }
    std::ranges::sort(index);
    // Z-DON: a song folder's write time from the loading walk's listing of its parent (no stat per
    // song); a folder the walk did not list (or one changed since) is stat'ed as before
    const std::shared_ptr<const LibraryIndex> walk = library_index();
    constexpr int64_t kUnixEpochIn1601 = 11644473600;
    const int64_t unix_cutoff =
        ch::duration_cast<ch::seconds>((ch::system_clock::now() - ch::weeks(2)).time_since_epoch()).count();
    const uint64_t ft_cutoff = static_cast<uint64_t>((unix_cutoff + kUnixEpochIn1601) * 10000000);
    fs::path listed_parent;
    std::unordered_map<std::wstring, uint64_t> listed_folders; // name -> ft_write, of listed_parent
    bool parent_listed = false;
    for (const auto& path_str : index) {
        if (abort_loading) {
            break;
        }
        fs::path song_path(path_str);
        fs::path rel = song_path.lexically_relative(parent);
        if (rel.empty() || rel.begin()->string() == "..") {
            continue;
        }
        fs::path sibling = parent / *rel.begin();
        if (sibling == path) {
            continue;
        }
        const fs::path folder = song_path.parent_path();
        if (walk && folder.parent_path() != listed_parent) {
            listed_parent = folder.parent_path();
            listed_folders.clear();
            const LibraryIndex::Dir* d = walk->find(listed_parent);
            parent_listed = d != nullptr;
            if (d) {
                for (const LibraryIndex::Entry& e : d->entries) {
                    if (e.is_dir && !e.is_reparse && e.ft_write != 0) {
                        listed_folders.emplace(e.name, e.ft_write);
                    }
                }
            }
        }
        if (auto it = parent_listed ? listed_folders.find(folder.filename().wstring()) : listed_folders.end();
            it != listed_folders.end()) {
            if (it->second < ft_cutoff) {
                continue;
            }
        } else {
            std::error_code ec;
            auto last_write = fs::last_write_time(folder, ec);
            if (ec || last_write < two_weeks_ago) {
                continue;
            }
        }
        hits.push_back(song_path);
    }
    return hits;
}

// The hits and their parsers usually come from the prefetch started with the folder's open
// animation (begin_inline_load); the sibling of the collection folder supplies the genre.
void Navigator::load_collection_new(const fs::path& path, const BoxDef& box_def) {
    std::vector<fs::path> hits;
    std::unordered_map<std::string, std::unique_ptr<SongParser>> preparsed;
    join_prefetch();
    // join_loader aborts a prefetch still running when the animation ends: its list may be cut short
    if (prefetch && prefetch->new_songs && prefetch->complete && prefetch->path == path && !abort_loading) {
        hits = std::move(prefetch->song_paths);
        preparsed = std::move(prefetch->preparsed);
        prefetch.reset();
    } else {
        prefetch.reset();
        wait_for_song_files();
        hits = find_new_songs(path);
        preparsed = parse_songs_parallel(hits, abort_loading);
    }

    const fs::path parent = path.parent_path();
    int songs_added = 0;
    for (const auto& h : hits) {
        if (abort_loading) {
            break;
        }
        enqueue_inline_back_every_ten(songs_added, parent);
        auto song = make_song_box(h, box_def, take_parser(preparsed, h));
        apply_song_genre(song.get(), parse_box_def(parent / *h.lexically_relative(parent).begin()));
        song->fade_in(266);
        enqueue_inline_box(std::move(song));
        songs_added++;
    }
}

// Difficulty filter: the startup scan already parsed every chart, so the course levels come
// from song_levels (no disk parse per song); only the hits are parsed, in parallel, for their
// boxes. Songs live under the siblings of the collection folder; the sibling (first path
// component below their common parent) supplies the genre.
void Navigator::load_collection_difficulty(const fs::path& path, const BoxDef& box_def, int course, int level,
                                           int order) {
    struct Hit {
        fs::path path;
        fs::path sibling;
        bool unmet;
    };
    std::vector<Hit> hits;

    const int crown_needed = (order == 2)   ? static_cast<int>(Crown::CLEAR)
                             : (order == 3) ? static_cast<int>(Crown::FC)
                             : (order == 4) ? static_cast<int>(Crown::DFC)
                                            : 0;

    wait_for_song_files();
    const fs::path parent = path.parent_path();
    // the scan thread is joined above, so song_levels is read without a lock
    std::vector<std::pair<std::string, std::array<int, 5>>> index(song_levels.begin(), song_levels.end());
    std::ranges::sort(index, [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [path_str, levels] : index) {
        if (abort_loading) {
            break;
        }
        if (course < 0 || course > 4 || levels[course] != level) {
            continue;
        }
        fs::path song_path(path_str);
        // the song must sit below one of the collection folder's siblings
        fs::path rel = song_path.lexically_relative(parent);
        if (rel.empty() || rel.begin()->string() == "..") {
            continue;
        }
        fs::path sibling = parent / *rel.begin();
        if (sibling == path || !fs::is_directory(sibling)) {
            continue;
        }

        bool unmet = false;
        if (crown_needed > 0) {
            unmet = true;
            const auto& hashes = scores_manager.get_hashes(song_path);
            const std::string& hash = hashes[course];
            if (!hash.empty()) {
                auto score = scores_manager.get_score(hash, course, scores_manager.player_1);
                if (score.has_value() && static_cast<int>(score->crown) >= crown_needed) {
                    unmet = false;
                }
            }
        }
        hits.push_back(Hit{song_path, sibling, unmet});
    }

    if (crown_needed > 0) {
        if (global_data.config->general.zdon_sort_filter) {
            // Z-DON: "not cleared / not full combo / not donderful" lists only those songs
            std::erase_if(hits, [](const Hit& h) { return !h.unmet; });
        } else {
            std::ranges::stable_partition(hits, [](const Hit& h) { return h.unmet; });
        }
    }

    std::vector<fs::path> hit_paths;
    hit_paths.reserve(hits.size());
    for (const auto& h : hits) {
        hit_paths.push_back(h.path);
    }
    auto preparsed = parse_songs_parallel(hit_paths, abort_loading);

    int songs_added = 0;
    for (const auto& h : hits) {
        if (abort_loading) {
            break;
        }
        enqueue_inline_back_every_ten(songs_added, parent);
        auto song = make_song_box(h.path, box_def, take_parser(preparsed, h.path));
        apply_song_genre(song.get(), parse_box_def(h.sibling));
        song->fade_in(266);
        enqueue_inline_box(std::move(song));
        songs_added++;
    }
}

void Navigator::apply_diff_sort(int course, int level, int order) {
    diff_sort_filter = std::array<int, 3>{course, level, order};
    awaiting_diff_sort = false;
    begin_inline_load();
}

void Navigator::cancel_diff_sort() {
    awaiting_diff_sort = false;
    diff_sort_filter.reset();
    inline_state.reset();
    pending_inline_path.reset();
    pending_inline_folder = nullptr;
}

void Navigator::load_from_song_list(const fs::path& path, const BoxDef& box_def, bool mark_favorite) {
    int songs_added = 0;
    for (const auto& entry : read_song_list(song_list_file(path))) {
        if (abort_loading) {
            break;
        }
        fs::path song_path;
        if (auto found = scores_manager.get_path_by_hash(entry.hash)) {
            song_path = *found;
        }
        if (!entry.title.empty() || !entry.subtitle.empty()) {
            auto by_title = song_files.find({entry.title, entry.subtitle});
            if (by_title != song_files.end() && scores_manager.get_single_hash(by_title->second) == entry.hash) {
                song_path = by_title->second;
            }
        }
        if (song_path.empty()) {
            continue;
        }
        enqueue_inline_back_every_ten(songs_added, path.parent_path());
        std::unordered_map<std::string, std::unique_ptr<SongParser>> none;
        auto song = make_song_box(song_path, box_def, take_parser(none, song_path));
        song->preserve_order = true;
        if (mark_favorite) {
            song->is_favorite = true;
        }
        apply_genre_of(song_path, song.get());
        song->fade_in(266);
        enqueue_inline_box(std::move(song));
        songs_added++;
    }
}

static void push_song_list(const std::vector<SongListEntry>& entries, const char* kind) {
    const std::string account = song_list_account();
    if (account.empty()) {
        return;
    }
    std::vector<std::string> hashes;
    hashes.reserve(entries.size());
    for (const auto& entry : entries) {
        hashes.push_back(entry.hash);
    }
    std::thread([account, kind = std::string(kind), hashes = std::move(hashes)] {
        network.update_song_list(account, kind, hashes);
    }).detach();
}

void Navigator::toggle_favorite(SongBox* song) {
    if (!favorite_folder_path || hide_collections) {
        return;
    }
    FolderBox::invalidate_scan_cache(); // song_list.txt counts change
    std::string title = en_or_first(song->parser.metadata.title);
    std::string subtitle = en_or_first(song->parser.metadata.subtitle);
    std::string key = title + "|" + subtitle;

    fs::path song_list_path = song_list_file(*favorite_folder_path);
    auto entries = read_song_list(song_list_path);

    bool was_favorite = favorite_songs.contains(key);
    if (was_favorite) {
        favorite_songs.erase(key);
        std::erase_if(entries, [&](const SongListEntry& e) { return e.title == title && e.subtitle == subtitle; });
        song->is_favorite = false;
        notify("Removed from favorites");
    } else {
        favorite_songs.insert(key);
        std::string hash = scores_manager.get_single_hash(song->parser.file_path);
        entries.insert(entries.begin(), {std::move(hash), title, subtitle});
        song->is_favorite = true;
        notify("Added to favorites");
    }

    queue_song_list_write(song_list_path, entries);
    push_song_list(entries, "favorites");
}

fs::path Navigator::find_box_def_folder(const fs::path& song_path) {
    fs::path current = song_path.parent_path();
    while (!current.empty() && current != current.parent_path()) {
        if (fs::exists(current / "box.def")) {
            return current;
        }
        current = current.parent_path();
    }
    return fs::path{};
}

void Navigator::add_to_recent(const SongBox* song) {
    if (!recent_folder_path || hide_collections) {
        return;
    }
    FolderBox::invalidate_scan_cache(); // song_list.txt counts change
    fs::path song_list_path = song_list_file(*recent_folder_path);

    auto& titles = song->parser.metadata.title;
    auto& subtitles = song->parser.metadata.subtitle;
    std::string title = titles.contains("en") ? titles.at("en") : titles.begin()->second;
    std::string subtitle = subtitles.contains("en") ? subtitles.at("en") : subtitles.begin()->second;
    std::string hash = scores_manager.get_single_hash(song->parser.file_path);

    auto entries = read_song_list(song_list_path);
    std::erase_if(entries, [&](const SongListEntry& e) { return e.title == title && e.subtitle == subtitle; });
    entries.insert(entries.begin(), {std::move(hash), title, subtitle});
    if (static_cast<int>(entries.size()) > 25) {
        entries.resize(25);
    }

    queue_song_list_write(song_list_path, entries);
    push_song_list(entries, "recent");
    promote_recent_box(song);
}

void Navigator::promote_recent_box(const SongBox* song) {
    if (!inline_state.has_value() || pending_inline_folder == nullptr) {
        return;
    }
    if (pending_inline_folder->collection != "RECENT") {
        return;
    }

    int first = inline_state->first_song_index;
    int end = first + inline_state->songs_count;
    if (first < 0 || end > static_cast<int>(items.size())) {
        return;
    }

    std::vector<int> slots;
    for (int i = first; i < end; i++) {
        if (dynamic_cast<SongBox*>(items[i].get())) {
            slots.push_back(i);
        }
    }

    int pos = -1;
    for (int i = 0; i < static_cast<int>(slots.size()); i++) {
        if (items[slots[i]]->path == song->path) {
            pos = i;
            break;
        }
    }
    if (pos <= 0) {
        return; // not in this listing, or already first
    }

    auto moved = std::move(items[slots[pos]]);
    for (int i = pos; i > 0; i--) {
        items[slots[i]] = std::move(items[slots[i - 1]]);
    }
    items[slots[0]] = std::move(moved);

    // Keep the cursor on the same song rather than on whatever slid into
    // its slot.
    if (open_index == slots[pos]) {
        open_index = slots[0];
    } else {
        for (int i = 0; i < pos; i++) {
            if (open_index == slots[i]) {
                open_index = slots[i + 1];
                break;
            }
        }
    }
}

void Navigator::load_collection_recommended(const fs::path& path, const BoxDef& box_def) {
    std::vector<fs::path> all_songs;
    all_songs.reserve(song_files.size());
    for (const auto& [key, song_path] : song_files) {
        all_songs.push_back(song_path);
    }

    std::mt19937 rng(fixed_step::seed());
    std::shuffle(all_songs.begin(), all_songs.end(), rng);
    int count = std::min(static_cast<int>(all_songs.size()), 10);
    for (int i = 0; i < count; i++) {
        if (abort_loading) {
            break;
        }
        const fs::path& song_path = all_songs[i];
        std::unordered_map<std::string, std::unique_ptr<SongParser>> none;
        auto song = make_song_box(song_path, box_def, take_parser(none, song_path));
        song->preserve_order = true;
        apply_genre_of(song_path, song.get());
        song->fade_in(266);
        enqueue_inline_box(std::move(song));
    }
}

void Navigator::load_collection_search(const fs::path& path, const BoxDef& box_def) {
    if (current_search.empty()) {
        return;
    }
    const std::string query = search_fold(current_search);
    // Match any title / subtitle in any language (the box key is the English pair only), then
    // parse the hits in parallel: a short query can match hundreds of songs
    std::vector<fs::path> hits;
    for (const auto& [key, song_path] : song_files) {
        if (abort_loading) {
            break;
        }
        auto st = song_search_text.find(song_path.string());
        const std::string& text = (st != song_search_text.end()) ? st->second : search_fold(key.first);
        if (text.contains(query)) {
            hits.push_back(song_path);
        }
    }
    auto preparsed = parse_songs_parallel(hits, abort_loading);

    int songs_added = 0;
    for (const fs::path& song_path : hits) {
        if (abort_loading) {
            break;
        }
        enqueue_inline_back_every_ten(songs_added, path.parent_path());
        auto song = make_song_box(song_path, box_def, take_parser(preparsed, song_path));
        apply_genre_of(song_path, song.get());
        song->fade_in(266);
        enqueue_inline_box(std::move(song));
        songs_added++;
    }
}

// The player's most played songs, most plays first
void Navigator::load_collection_play_count(const fs::path& path, const BoxDef& box_def) {
    std::unordered_map<fs::path, int> plays;
    for (const auto& [hash, count] : scores_manager.get_play_counts(scores_manager.player_1)) {
        if (auto song_path = scores_manager.get_path_by_diff_hash(hash)) {
            plays[*song_path] += count;
        }
    }
    std::vector<std::pair<fs::path, int>> hits(plays.begin(), plays.end());
    std::ranges::sort(hits, [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    std::vector<fs::path> hit_paths;
    hit_paths.reserve(hits.size());
    for (const auto& h : hits) {
        hit_paths.push_back(h.first);
    }
    auto preparsed = parse_songs_parallel(hit_paths, abort_loading);

    int songs_added = 0;
    for (const fs::path& song_path : hit_paths) {
        if (abort_loading) {
            break;
        }
        enqueue_inline_back_every_ten(songs_added, path.parent_path());
        auto song = make_song_box(song_path, box_def, take_parser(preparsed, song_path));
        song->preserve_order = true;
        apply_genre_of(song_path, song.get());
        song->fade_in(266);
        enqueue_inline_box(std::move(song));
        songs_added++;
    }
}

void Navigator::scan_song_tree(const fs::path& path, std::vector<fs::path>& song_paths,
                               std::unordered_map<std::string, std::vector<std::pair<bool, fs::path>>>& plan) {
    // Collect the song files with ONE pass per directory, reading the type off the
    // directory entry itself (the listing already carries it) instead of stat()ing every
    // path again, and deciding "osu folder" from the same listing instead of re-listing
    // each folder through is_osu_song_folder().  On a 600-song genre the old walk cost
    // ~4 stats per file plus one extra enumeration per folder -- most of the open time.
    //
    // A folder the startup walk listed is read from the library index instead (no disk access);
    // entries are typed the way directory_entry::is_directory / is_regular_file type them.
    struct Typed {
        fs::path path;
        bool is_dir;
        bool is_regular;
        const LibraryIndex::Dir* listing; // the folder's own listing in the index, if any
    };
    const std::shared_ptr<const LibraryIndex> index = library_index();
    std::error_code ec;
    // the entries of `dir` in listing order, until an abort
    auto list = [&](const fs::path& dir, const LibraryIndex::Dir* listing, std::vector<Typed>& out) {
        out.clear();
        if (!listing && index) {
            listing = index->find(dir);
        }
        if (listing) {
            for (const LibraryIndex::Entry& e : listing->entries) {
                if (abort_loading) {
                    return;
                }
                fs::path p = dir / e.name;
                if (e.is_reparse) {
                    std::error_code tec;
                    const bool d = fs::is_directory(p, tec);
                    const bool r = !d && fs::is_regular_file(p, tec);
                    out.push_back({std::move(p), d, r, index->child(e)});
                } else {
                    out.push_back({std::move(p), e.is_dir, !e.is_dir, index->child(e)});
                }
            }
            return;
        }
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (abort_loading) {
                return;
            }
            const fs::directory_entry& e = *it;
            std::error_code tec;
            const bool d = e.is_directory(tec);
            const bool r = !d && e.is_regular_file(tec);
            out.push_back({e.path(), d, r, nullptr});
        }
        ec.clear();
    };
    // plan[top-level dir] = what sits under it, in walk order: {false, song file} or {true, osu folder}.
    // The second pass below replays this instead of walking the tree again.
    struct Work {
        fs::path dir;    // to list
        std::string key; // top-level key
        const LibraryIndex::Dir* listing;
    };
    std::vector<Work> work;
    std::vector<Typed> entries;
    auto scan_dir = [&](const fs::path& dir, const std::string& key, const LibraryIndex::Dir* listing) {
        std::vector<fs::path> here;
        std::vector<Typed> subdirs;
        bool is_osu = false;
        list(dir, listing, entries);
        if (abort_loading) {
            return;
        }
        for (Typed& e : entries) {
            if (e.is_dir) {
                subdirs.push_back(std::move(e));
                continue;
            }
            if (!e.is_regular) {
                continue;
            }
            auto ext = e.path.extension();
            if (ext == ".osu") {
                is_osu = true;
            }
            if (ext == ".tja" || ext == ".osu") {
                here.push_back(std::move(e.path));
            }
        }
        if (is_osu) {
            plan[key].emplace_back(true, dir);
            return;
        } // one box per osu folder, no recursion
        for (const auto& f : here) {
            song_paths.push_back(f);
            plan[key].emplace_back(false, f);
        }
        for (Typed& d : subdirs) {
            work.push_back({std::move(d.path), key, d.listing});
        }
    };
    list(path, nullptr, entries);
    for (Typed& e : entries) {
        if (e.is_dir) {
            std::string key = e.path.string();
            plan[key];
            work.push_back({std::move(e.path), key, e.listing});
        } else if (e.is_regular) {
            auto ext = e.path.extension();
            if (ext == ".tja" || ext == ".osu") {
                song_paths.push_back(e.path);
            }
        }
    }
    for (size_t i = 0; i < work.size() && !abort_loading; i++) {
        // a copy: scan_dir appends to `work`, which can move the element a reference would point at
        const Work item = work[i]; // NOLINT(performance-unnecessary-copy-initialization)
        scan_dir(item.dir, item.key, item.listing);
    }
}

void Navigator::start_inline_prefetch(const fs::path& path) {
    join_prefetch();
    prefetch = std::make_unique<InlinePrefetch>();
    prefetch->path = path;
    InlinePrefetch* pf = prefetch.get();
    prefetch_thread = std::thread([this, pf] {
        scan_song_tree(pf->path, pf->song_paths, pf->plan);
        if (!abort_loading) {
            pf->preparsed = parse_songs_parallel(pf->song_paths, abort_loading, jobs::Priority::Normal);
        }
    });
    fixed_step::join(prefetch_thread);
}

// Only once the startup scan is done: the prefetch thread must not join song_files_thread (the
// loader does) and reads song_levels without a lock.
void Navigator::start_new_songs_prefetch(const fs::path& path) {
    join_prefetch();
    prefetch = std::make_unique<InlinePrefetch>();
    prefetch->path = path;
    prefetch->new_songs = true;
    InlinePrefetch* pf = prefetch.get();
    prefetch_thread = std::thread([this, pf] {
        pf->song_paths = find_new_songs(pf->path);
        if (!abort_loading) {
            pf->preparsed = parse_songs_parallel(pf->song_paths, abort_loading, jobs::Priority::Normal);
        }
        pf->complete = !abort_loading;
    });
    fixed_step::join(prefetch_thread);
}

void Navigator::join_prefetch() {
    if (prefetch_thread.joinable()) {
        prefetch_thread.join();
    }
}

void Navigator::load_songs_inline_async(const fs::path path, BoxDef box_def) {
    if (load_gen4_genre_songs(path, box_def)) {
        loading_complete = true;
        return;
    }
    if (load_gen3_genre_songs(path, box_def)) {
        loading_complete = true;
        return;
    }

    wait_for_song_files();
    int songs_added = 0;
    std::unordered_map<std::string, std::unique_ptr<SongParser>> preparsed;

    auto add_song = [&](const fs::path& song_path) {
        enqueue_inline_back_every_ten(songs_added, path.parent_path());
        auto box = make_song_box(song_path, box_def, take_parser(preparsed, song_path));
        box->fade_in(266);
        enqueue_inline_box(std::move(box));
        songs_added++;
    };

    // a virtual collection: run its loader, log (not rethrow) a failure, and finish the load
    auto run_collection = [&](const char* kind, auto&& load) {
        try {
            load();
        } catch (const std::exception& e) {
            spdlog::error("Error loading {} collection {}: {}", kind, path.string(), e.what());
        }
        loading_complete = true;
    };

    if (box_def.collection == "RECOMMENDED") {
        run_collection("recommended", [&] { load_collection_recommended(path, box_def); });
        return;
    }

    if (box_def.collection == "FAVORITE") {
        run_collection("favorite", [&] { load_from_song_list(path, box_def, true); });
        return;
    }

    if (box_def.collection == "RECENT") {
        run_collection("recent", [&] { load_from_song_list(path, box_def, false); });
        return;
    }

    if (box_def.collection == "DIFFICULTY") {
        if (diff_sort_filter) {
            try {
                load_collection_difficulty(path, box_def, (*diff_sort_filter)[0], (*diff_sort_filter)[1],
                                           (*diff_sort_filter)[2]);
            } catch (const std::exception& e) {
                spdlog::error("Error loading difficulty collection {}: {}", path.string(), e.what());
            }
            diff_sort_filter.reset();
        }
        loading_complete = true;
        return;
    }

    if (box_def.collection == "NEW") {
        run_collection("new", [&] { load_collection_new(path, box_def); });
        return;
    }

    if (box_def.collection == "SEARCH") {
        run_collection("search", [&] { load_collection_search(path, box_def); });
        return;
    }

    if (box_def.collection == "PLAY_COUNT") {
        run_collection("play count", [&] { load_collection_play_count(path, box_def); });
        return;
    }

    std::vector<fs::path> song_paths;
    std::unordered_map<std::string, std::vector<std::pair<bool, fs::path>>> plan;
    join_prefetch();
    if (prefetch && !prefetch->new_songs && prefetch->path == path && !abort_loading) {
        song_paths = std::move(prefetch->song_paths);
        plan = std::move(prefetch->plan);
        preparsed = std::move(prefetch->preparsed);
        prefetch.reset();
    } else {
        prefetch.reset();
        scan_song_tree(path, song_paths, plan);
        preparsed = parse_songs_parallel(song_paths, abort_loading);
    }

    try {
        for (const fs::directory_entry& entry : fs::directory_iterator(path)) {
            if (abort_loading) {
                break;
            }
            const fs::path& curr_path = entry.path();
            try {
                std::error_code tec;
                if (!entry.is_directory(tec)) {
                    if (is_song_list_file(curr_path)) {
                        parse_song_list(curr_path, box_def, true);
                        continue;
                    }
                    if (is_song_file(curr_path)) {
                        enqueue_inline_box(make_song_box(curr_path, box_def, take_parser(preparsed, curr_path)));
                    }
                    continue;
                }
                auto pit = plan.find(curr_path.string());
                if (pit == plan.end()) {
                    continue; // not seen by the scan (removed meanwhile)
                }
                for (const auto& [is_osu, p] : pit->second) {
                    if (abort_loading) {
                        break;
                    }
                    try {
                        if (is_osu) {
                            BoxDef osu_box_def = box_def;
                            osu_box_def.name = p.filename().string();
                            auto folder = std::make_unique<FolderBox>(p, osu_box_def);
                            folder->fade_in(266);
                            enqueue_inline_box(std::move(folder));
                        } else {
                            add_song(p);
                        }
                    } catch (const std::exception& inner) {
                        spdlog::warn("Skipping song: {}", inner.what());
                    }
                }
            } catch (const std::exception& e) {
                spdlog::warn("Skipping entry: {}, {}", e.what(), curr_path.string());
            }
        }
    } catch (const fs::filesystem_error& e) {
        spdlog::error("Error loading directory: {}", e.what());
    }
    loading_complete = true;
}

BoxDef Navigator::parse_box_def(const fs::path& path) {
    std::string key = path.string();
    auto it = box_def_cache.find(key);
    if (it != box_def_cache.end()) {
        return it->second;
    }
    BoxDef result = parse_box_def_uncached(path);
    box_def_cache[key] = result;
    return result;
}

BoxDef Navigator::parse_box_def_uncached(const fs::path& path) {
    // merge 1cd3844: the one box.def parser (navigation_load.cpp), with upstream #362's colour stops / textures
    return parse_navigation_box_def(path, global_data.config->general.language);
}

bool Navigator::has_def_file(const std::filesystem::path& path) {
    std::string key = path.string();
    {
        std::scoped_lock lock(child_folders_mutex);
        auto it = def_file_cache.find(key);
        if (it != def_file_cache.end()) {
            return it->second;
        }
    }
    auto remember = [&](bool result) {
        std::scoped_lock lock(child_folders_mutex);
        def_file_cache[key] = result;
        return result;
    };

    // From the library index when the startup walk listed the folder: the same answer without a stat
    // (the index is only built on Windows)
#ifdef _WIN32
    if (auto index = library_index()) {
        if (const LibraryIndex::Dir* listing = index->find(path)) {
            for (const LibraryIndex::Entry& e : listing->entries) {
                if (_wcsicmp(e.name.c_str(), L"box.def") == 0) {
                    std::error_code ec;
                    if (!e.is_reparse || fs::exists(path / e.name, ec)) {
                        return remember(true);
                    }
                }
            }
            for (const LibraryIndex::Entry& e : listing->entries) {
                const fs::path sub = path / e.name;
                std::error_code ec;
                if (e.is_reparse ? !fs::is_directory(sub, ec) : !e.is_dir) {
                    continue;
                }
                if (is_gen4_root(sub) || !gen3_root_at(sub).empty()) {
                    continue;
                }
                if (has_def_file(sub)) {
                    return remember(true);
                }
            }
            return remember(false);
        }
    }
#endif

    std::error_code ec;
    if (fs::exists(path / "box.def", ec)) {
        return remember(true);
    }

    for (const auto& entry : fs::directory_iterator(path, fs::directory_options::skip_permission_denied, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        if (is_gen4_root(entry.path()) || !gen3_root_at(entry.path()).empty()) {
            continue;
        }
        if (has_def_file(entry.path())) {
            return remember(true);
        }
    }
    return remember(false);
}

void Navigator::exit_inline() {
    if (!inline_state) {
        return;
    }
    emit_wheel_event(WHEEL_EVENT_CLOSE_BEGIN);
    InlineState& state = *inline_state;

    // Kick off fade_out on all inserted items
    int end = state.first_song_index + state.songs_count;
    for (int i = std::max(0, state.first_song_index - 1); i < end; i++) {
        items[i]->fade_out(250);
    }

    state.fading_out = true;
    if (pending_inline_folder) {
        pending_inline_folder->exit_box();
    }
    if (genre_bg) {
        genre_bg->close_sides();
    }
    is_inline = false;
    is_processing = true;
}

void Navigator::begin_inline_load() {
    emit_wheel_event(WHEEL_EVENT_OPEN_BEGIN);
    if (pending_inline_path && pending_inline_box_def.collection.empty()) {
        start_inline_prefetch(*pending_inline_path);
    } else if (pending_inline_path && pending_inline_box_def.collection == "NEW" && song_files_ready) {
        start_new_songs_prefetch(*pending_inline_path);
    }
    bg_genre_pending = false;
    items[open_index]->enter_box();
    genre_bg_start = open_index;
    genre_bg_end = open_index;
    genre_bg.emplace(items[open_index]->text_name, items[open_index]->back_color, items[open_index]->texture_index);
    is_processing = true;
}

void Navigator::load_current_directory(const fs::path path) {
    BoxDef box_def = parse_box_def(path);
    bool has_children = has_child_folders(path);

#ifdef SUPPORT_FUMEN
    bool own_path_is_arcade_root = !gen4::find_data_root(path).empty() || !gen3::find_data_root(path).empty();
#else
    bool own_path_is_arcade_root = false;
#endif
    if (has_children && root_paths.size() > 1 && !reloading_roots && std::ranges::contains(root_paths, path) &&
        !own_path_is_arcade_root) {
        if (inline_state.has_value()) {
            collapse_inline_now();
            return;
        }

        {
            fs::path game;
#ifdef SUPPORT_FUMEN
            game = gen4::find_data_root(current_path);
            if (game.empty()) {
                game = gen3::find_data_root(current_path);
            }
#endif
            if (!game.empty()) {
                restore_cursor_path = game;
            } else if (open_index >= 0 && open_index < static_cast<int>(items.size())) {
                restore_cursor_path = items[open_index]->path;
            }
        }
        load_all_roots();
        return;
    }

    if (!has_children && !items.empty()) {
        if (inline_state.has_value()) {
            collapse_inline_now();
            items[open_index]->close_box();
            for (int i = 0; i < static_cast<int>(items.size()); i++) {
                if (items[i]->path == path) {
                    open_index = i;
                    break;
                }
            }
            set_positions(true, 0);
        }

        InlineState state;
        state.folder_index = open_index;
        state.first_song_index = open_index + 1;
        state.songs_count = 0;
        pending_inline_folder = static_cast<FolderBox*>(items[open_index].get());
        inline_state = std::move(state);
        pending_inline_path = path;
        pending_inline_box_def = box_def;

        if (box_def.collection == "DIFFICULTY") {
            diff_sort_scope.reset(); // merge 09db50f: upstream's per-folder scope needs its navigation loader
            awaiting_diff_sort = true;
            return;
        }

        begin_inline_load();
        return;
    }

    if (is_inline) {
        exit_inline();
        return;
    }

    open_index = 0;
    clear_folder_caches();
    setup_back_box(path, true);
    is_processing = true;
    loading_complete = false;

    join_loader();
#ifndef __EMSCRIPTEN__
    loader_thread = std::thread(&Navigator::load_current_directory_async, this, path);
    fixed_step::join(loader_thread);
#else
    load_current_directory_async(path);
#endif
}

bool Navigator::jump_to_song(const std::string& hash) {
    auto song_path_opt = scores_manager.get_path_by_diff_hash(hash);
    if (!song_path_opt) {
        spdlog::warn("jump_to_song: no song found for hash {}", hash);
        return false;
    }
    return jump_to_song_path(*song_path_opt);
}

bool Navigator::jump_to_song_path(const fs::path& song_path) {
    std::vector<fs::path> chain;
    fs::path cur = song_path.parent_path();
    while (true) {
        fs::path folder = find_box_def_folder(cur);
        if (folder.empty()) {
            break;
        }
        chain.push_back(folder);
        if (std::ranges::contains(root_paths, folder.parent_path())) {
            break;
        }
        cur = folder.parent_path();
    }
    if (chain.empty()) {
        spdlog::warn("jump_to_song: could not resolve a genre folder for {}", song_path.string());
        return false;
    }
    std::ranges::reverse(chain);

    BoxDef final_box_def = parse_box_def(chain.back());
    if (!final_box_def.collection.empty()) {
        spdlog::warn("jump_to_song: song resolves into a synthetic collection folder, not supported: {}",
                     chain.back().string());
        return false;
    }

    join_loader();
    is_inline = false;
    inline_state.reset();
    pending_inline_path.reset();
    pending_inline_folder = nullptr;
    genre_bg.reset();
    awaiting_diff_sort = false;
    diff_sort_filter.reset();

    auto load_full = [&](const fs::path& folder) {
        open_index = 0;
        clear_folder_caches();
        setup_back_box(folder, true);
        loading_complete = false;
        load_current_directory_async(folder);
        flush_pending_boxes();
    };

    load_full(chain.front().parent_path());
    for (size_t i = 0; i + 1 < chain.size(); i++) {
        load_full(chain[i]);
    }

    fs::path final_folder = chain.back();
    bool final_has_children = has_child_folders(final_folder);

    if (final_has_children || items.empty()) {
        load_full(final_folder);
    } else {
        int folder_index = -1;
        for (int i = 0; i < static_cast<int>(items.size()); i++) {
            if (items[i]->path == final_folder) {
                folder_index = i;
                break;
            }
        }
        if (folder_index < 0) {
            spdlog::warn("jump_to_song: could not locate folder box for {}", final_folder.string());
            return false;
        }

        items[open_index]->close_box();
        open_index = folder_index;
        set_positions(true, 0);
        items[open_index]->expand_box();
        items[open_index]->enter_box();

        FolderBox* folder_box = static_cast<FolderBox*>(items[open_index].get());
        for (auto& item : items) {
            if (item && item->position > folder_box->position) {
                item->move_box(tex.screen_width + 150, 600);
            }
        }

        InlineState state;
        state.folder_index = open_index;
        state.first_song_index = open_index + 1;
        state.songs_count = 0;
        state.saved_folder_box = std::unique_ptr<FolderBox>(static_cast<FolderBox*>(items[open_index].release()));
        pending_inline_folder = folder_box;
        inline_state = std::move(state);

        setup_back_box(final_folder, false, folder_box);
        genre_bg.emplace(folder_box->text_name, folder_box->back_color, folder_box->texture_index);
        genre_bg->settle();

        loading_complete = false;
        is_inline = true;
        load_songs_inline_async(final_folder, final_box_def);
        flush_pending_boxes();
    }

    if (items.empty()) {
        spdlog::warn("jump_to_song: destination folder loaded no items for {}", song_path.string());
        return false;
    }

    int target_index = -1;
    for (int i = 0; i < static_cast<int>(items.size()); i++) {
        if (items[i]->path == song_path) {
            target_index = i;
            break;
        }
    }
    if (target_index < 0) {
        spdlog::warn("jump_to_song: song loaded but not found among items: {}", song_path.string());
        return false;
    }

    items[open_index]->close_box();
    open_index = target_index;
    bg_genre_index = items[open_index]->genre_index;
    last_bg_genre_index = bg_genre_index;
    bg_genre_pending = false;
    set_positions(true, 0);
    items[open_index]->expand_box();
    background_fade_change->start();
    is_processing = false;
    return true;
}

void Navigator::setup_back_box(const fs::path& path, bool has_children, const BaseBox* from) {
    // The box that opened this folder is the authority on its genre and colours: a
    // folder without box.def gets them from its name or the gen3/gen4 tables, which
    // parse_box_def(path) alone does not see.
    if (!from) {
        for (auto& b : items) {
            if (b && b->path == path) {
                from = b.get();
                break;
            }
        }
    }
    BoxDef folder = parse_box_def(path);
    if (from) {
        folder.genre_index = from->genre_index;
    }
    // the loaders repeat a back box every ten songs; those wear the same genre
    inline_back_def = folder;
    if (has_children) {
        if (!reloading_roots) {
            items.clear();
        }
        if (std::ranges::contains(root_paths, path) || path == start_folder) {
            return;
        }
        items.push_back(make_back_box(path.parent_path(), &folder));
    } else {
        auto back = make_back_box(path.parent_path(), &folder);
        back->fade_in(400, 400);
        items.erase(items.begin() + open_index);
        items.insert(items.begin() + open_index, std::move(back));
    }
}

// Asked again for the same folder on every open and close (closing a folder asks it of the parent).
bool Navigator::has_child_folders(const fs::path& path) {
    const std::string key = path.string();
    {
        std::scoped_lock lock(child_folders_mutex);
        if (auto it = child_folders_cache.find(key); it != child_folders_cache.end()) {
            return it->second;
        }
    }
    const bool result = scan_child_folders(path);
    std::scoped_lock lock(child_folders_mutex);
    child_folders_cache[key] = result;
    return result;
}

// Loader thread: answer has_child_folders for every folder under `parent` ahead of time. Opening a
// genre of several hundred songs otherwise lists each song folder on the main thread first (~80 ms
// warm, far longer from a cold disk) before the folder starts to open.
void Navigator::prefetch_child_folders(const fs::path& parent) {
    auto prefetch = [&](const fs::path& child) {
        try {
            has_child_folders(child);
        } catch (const std::exception& e) {
            spdlog::warn("Folder scan of {} failed: {}", child.string(), e.what());
        }
    };
    if (auto index = library_index()) {
        if (const LibraryIndex::Dir* listing = index->find(parent)) {
            for (const LibraryIndex::Entry& e : listing->entries) {
                if (abort_loading) {
                    return;
                }
                const fs::path child = parent / e.name;
                std::error_code dir_ec;
                if (e.is_reparse ? (!fs::is_directory(child, dir_ec) || dir_ec) : !e.is_dir) {
                    continue;
                }
                prefetch(child);
            }
            return;
        }
    }
    std::error_code ec;
    for (fs::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec)) {
        if (abort_loading) {
            return;
        }
        std::error_code dir_ec;
        if (!it->is_directory(dir_ec) || dir_ec) {
            continue;
        }
        prefetch(it->path());
    }
}

bool Navigator::scan_child_folders(const fs::path& path) {
#ifdef SUPPORT_FUMEN
    if (gen4::genre_of_path(path) >= 0) {
        return false;
    }
    if (!gen3::genre_of_path(path).empty()) {
        return false;
    }
#endif
    if (is_gen4_root(path) || is_gen3_root(path)) {
        return true;
    }

#ifdef _WIN32
    // The same checks from one listing per child folder: the portable loop listed each child twice and
    // stat'ed it about six times, which took most of a second for a genre of several hundred songs.
    // The listings come from the library index when the startup walk listed the folder.
    const std::shared_ptr<const LibraryIndex> index = library_index();
    std::vector<LibraryIndex::Entry> entries_storage, inner_storage;
    auto listing_of = [&](const fs::path& dir, const LibraryIndex::Dir* listed,
                          std::vector<LibraryIndex::Entry>& storage) -> const std::vector<LibraryIndex::Entry>* {
        if (!listed && index) {
            listed = index->find(dir);
        }
        if (listed) {
            return &listed->entries;
        }
        std::vector<Win32DirEntry> raw;
        if (!win32_list_dir(dir, raw)) {
            return nullptr;
        }
        storage.clear();
        for (Win32DirEntry& e : raw) {
            storage.push_back({std::move(e.name), e.is_dir, e.is_reparse, -1});
        }
        return &storage;
    };
    const std::vector<LibraryIndex::Entry>* entries = listing_of(path, nullptr, entries_storage);
    if (!entries) {
        throw fs::filesystem_error("cannot open directory", path,
                                   std::make_error_code(std::errc::no_such_file_or_directory));
    }
    auto named = [](const LibraryIndex::Entry& e, const wchar_t* name) { return _wcsicmp(e.name.c_str(), name) == 0; };
    for (const LibraryIndex::Entry& entry : *entries) {
        if (!entry.is_dir) {
            continue;
        }
        const fs::path child = path / entry.name;
        const std::vector<LibraryIndex::Entry>* inner_listing =
            listing_of(child, index ? index->child(entry) : nullptr, inner_storage);
        if (!inner_listing) {
            continue;
        }
        const std::vector<LibraryIndex::Entry>& inner = *inner_listing;

        bool has_fumen = false, has_usrdir = false;
        for (const LibraryIndex::Entry& e : inner) {
            if (named(e, L"box.def")) {
                return true; // has_def_file
            }
            if (!e.is_dir) {
                if (name_has_ext(e.name, L".osu")) {
                    return true; // is_osu_song_folder
                }
            } else if (named(e, L"fumen")) {
                has_fumen = true;
            } else if (named(e, L"USRDIR")) {
                has_usrdir = true;
            }
        }
        // a data root has a fumen folder (gen3 may sit under USRDIR/data)
        if ((has_fumen || has_usrdir) && (is_gen4_root(child) || !gen3_root_at(child).empty())) {
            return true;
        }
        // has_def_file: a box.def further down, not inside a data root
        for (const LibraryIndex::Entry& e : inner) {
            if (!e.is_dir) {
                continue;
            }
            const fs::path sub = child / e.name;
            if (is_gen4_root(sub) || !gen3_root_at(sub).empty()) {
                continue;
            }
            if (has_def_file(sub)) {
                return true;
            }
        }
    }
    return false;
#else
    return std::ranges::any_of(fs::directory_iterator(path), [this](const auto& entry) {
        return (fs::is_directory(entry.path()) && has_def_file(entry.path())) || is_osu_song_folder(entry.path()) ||
               is_gen4_root(entry.path()) || !gen3_root_at(entry.path()).empty();
    });
#endif
}

bool Navigator::is_directory(BaseBox* item) {
    if (item == nullptr) {
        return false;
    }
    if (dynamic_cast<FolderBox*>(item) != nullptr) {
        return true;
    }
    return !is_song(item) && fs::is_directory(item->path);
}

bool Navigator::is_song_file(const fs::path& path) {
    if (!fs::is_regular_file(path)) {
        return false;
    }
    auto ext = path.extension();
    return ext == ".tja" || ext == ".osu";
}

const BoxDef* Navigator::box_def_for_genre(GenreIndex genre) {
    if (!genre_box_defs_built) {
        genre_box_defs_built = true;
        for (const fs::path& root : root_paths) {
            std::error_code ec;
#ifdef SUPPORT_FUMEN
            if (!gen4::find_data_root(root).empty() || !gen3::find_data_root(root).empty()) {
                continue;
            }
#endif
            if (!fs::is_directory(root, ec)) {
                continue;
            }
            for (const auto& entry : fs::directory_iterator(root, ec)) {
                if (abort_loading) {
                    return nullptr;
                }
                if (!fs::is_directory(entry.path(), ec)) {
                    continue;
                }
                if (is_gen4_root(entry.path()) || !gen3_root_at(entry.path()).empty()) {
                    continue;
                }
                if (!has_def_file(entry.path())) {
                    continue;
                }
                BoxDef def = parse_box_def(entry.path());
                genre_box_defs.emplace(def.genre_index, def);
            }
        }
    }
    auto it = genre_box_defs.find(genre);
    return it != genre_box_defs.end() ? &it->second : nullptr;
}

bool Navigator::is_gen4_root(const fs::path& path) {
#ifdef SUPPORT_FUMEN
    std::error_code ec;
    if (!fs::is_directory(path, ec)) {
        return false;
    }
    return gen4::find_data_root(path) == path && gen4::library_for(path) != nullptr;
#else
    (void)path;
    return false;
#endif
}

bool Navigator::only_gen4_songs() {
#ifdef SUPPORT_FUMEN
    int games = 0;
    std::error_code ec;
    for (const fs::path& root : root_paths) {
        if (!gen4::find_data_root(root).empty() || !gen3::find_data_root(root).empty()) {
            games++;
            continue;
        }
        if (!fs::is_directory(root, ec)) {
            continue;
        }
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (is_gen4_root(entry.path()) || !gen3_root_at(entry.path()).empty()) {
                games++;
                continue;
            }
            return false;
        }
    }
    return games == 1;
#else
    return false;
#endif
}

void Navigator::load_gen4_genres(const fs::path& data_root) {
#ifdef SUPPORT_FUMEN
    const gen4::Library* library = gen4::library_for(data_root);
    if (!library) {
        return;
    }

    const std::string& lang = global_data.config->general.language;
    for (int genre_no : gen4::genres_present(*library)) {
        if (abort_loading) {
            break;
        }
        GenreIndex genre = (GenreIndex)gen4::genre_index_for(genre_no);

        BoxDef def;
        if (const BoxDef* like = box_def_for_genre(genre)) {
            def = *like;
        } else {
            def.texture_index = (genre == GenreIndex::VOCALOID) ? TextureIndex::VOCALOID : TextureIndex::NONE;
            if (auto colors = DEFAULT_COLORS.find(genre); colors != DEFAULT_COLORS.end()) {
                def.back_color = colors->second[0];
                def.fore_color = colors->second[1];
            }
        }
        def.name = gen4::genre_name(genre_no, lang);
        def.genre_index = genre;
        auto folder = std::make_unique<FolderBox>(gen4::genre_path(data_root, genre_no), def);
        folder->tja_count = 0;
        for (const gen4::OrderEntry& listing : library->order()) {
            if (listing.genre_no == genre_no) {
                folder->tja_count++;
            }
        }
        enqueue_box(std::move(folder));
    }
#else
    (void)data_root;
#endif
}

bool Navigator::load_gen4_genre_songs(const fs::path& path, const BoxDef& box_def) {
#ifdef SUPPORT_FUMEN
    int genre_no = gen4::genre_of_path(path);
    if (genre_no < 0) {
        return false;
    }

    fs::path data_root = gen4::find_data_root(path);
    const gen4::Library* library = gen4::library_for(data_root);
    if (!library) {
        return false;
    }

    GenreIndex genre = (GenreIndex)gen4::genre_index_for(genre_no);

    BoxDef def = box_def;
    if (const BoxDef* like = box_def_for_genre(genre)) {
        def = *like;
    } else if (auto colors = DEFAULT_COLORS.find(genre); colors != DEFAULT_COLORS.end()) {
        def.texture_index = (genre == GenreIndex::VOCALOID) ? TextureIndex::VOCALOID : TextureIndex::NONE;
        def.back_color = colors->second[0];
        def.fore_color = colors->second[1];
    }
    def.name = box_def.name;
    def.genre_index = genre;

    int songs_added = 0;
    for (const gen4::OrderEntry& listing : library->order()) {
        if (abort_loading) {
            break;
        }
        if (listing.genre_no != genre_no) {
            continue;
        }
        fs::path song_folder = data_root / "fumen" / listing.id;
        std::error_code ec;
        if (!fs::is_directory(song_folder, ec)) {
            continue;
        }
        enqueue_inline_back_every_ten(songs_added, data_root);
        auto box = make_song_box(song_folder, def, SongParser(song_folder));
        box->preserve_order = true;
        box->fade_in(266);
        enqueue_inline_box(std::move(box));
        songs_added++;
    }
    return true;
#else
    (void)path;
    (void)box_def;
    return false;
#endif
}

bool Navigator::is_gen3_root(const fs::path& path) {
#ifdef SUPPORT_FUMEN
    std::error_code ec;
    if (!fs::is_directory(path, ec)) {
        return false;
    }
    return gen3::find_data_root(path) == path && gen3::library_for(path) != nullptr;
#else
    (void)path;
    return false;
#endif
}

fs::path Navigator::gen3_root_at(const fs::path& path) {
#ifdef SUPPORT_FUMEN
    if (is_gen3_root(path)) {
        return path;
    }
    fs::path nested = path / "USRDIR" / "data";
    if (is_gen3_root(nested)) {
        return nested;
    }
    return {};
#else
    (void)path;
    return {};
#endif
}

bool Navigator::is_gen3_song_folder(const fs::path& path) {
#ifdef SUPPORT_FUMEN
    if (path.parent_path().filename() != "fumen") {
        return false;
    }
    const gen3::Library* library = gen3::library_for(path);
    return library && library->find(path.filename().string()) != nullptr;
#else
    (void)path;
    return false;
#endif
}

void Navigator::load_gen3_genres(const fs::path& data_root) {
#ifdef SUPPORT_FUMEN
    const gen3::Library* library = gen3::library_for(data_root);
    if (!library) {
        return;
    }

    for (const std::string& genre_name : gen3::genres_present(*library)) {
        if (abort_loading) {
            break;
        }
        GenreIndex genre = (GenreIndex)gen3::genre_index_for(genre_name);

        BoxDef def;
        if (const BoxDef* like = box_def_for_genre(genre)) {
            def = *like;
        } else {
            def.texture_index = (genre == GenreIndex::VOCALOID) ? TextureIndex::VOCALOID : TextureIndex::NONE;
            if (auto colors = DEFAULT_COLORS.find(genre); colors != DEFAULT_COLORS.end()) {
                def.back_color = colors->second[0];
                def.fore_color = colors->second[1];
            }
        }
        def.name = genre_name;
        def.genre_index = genre;
        auto folder = std::make_unique<FolderBox>(gen3::genre_path(data_root, genre_name), def);
        folder->tja_count = 0;
        for (const gen3::SongEntry& e : library->songs()) {
            if (e.genre == genre_name) {
                folder->tja_count++;
            }
        }
        enqueue_box(std::move(folder));
    }
#else
    (void)data_root;
#endif
}

bool Navigator::load_gen3_genre_songs(const fs::path& path, const BoxDef& box_def) {
#ifdef SUPPORT_FUMEN
    std::string genre_name = gen3::genre_of_path(path);
    if (genre_name.empty()) {
        return false;
    }

    fs::path data_root = gen3::find_data_root(path);
    const gen3::Library* library = gen3::library_for(data_root);
    if (!library) {
        return false;
    }

    GenreIndex genre = (GenreIndex)gen3::genre_index_for(genre_name);

    BoxDef def = box_def;
    if (const BoxDef* like = box_def_for_genre(genre)) {
        def = *like;
    } else if (auto colors = DEFAULT_COLORS.find(genre); colors != DEFAULT_COLORS.end()) {
        def.texture_index = (genre == GenreIndex::VOCALOID) ? TextureIndex::VOCALOID : TextureIndex::NONE;
        def.back_color = colors->second[0];
        def.fore_color = colors->second[1];
    }
    def.name = box_def.name;
    def.genre_index = genre;

    int songs_added = 0;
    for (const gen3::SongEntry& listing : library->songs()) {
        if (abort_loading) {
            break;
        }
        if (listing.genre != genre_name) {
            continue;
        }
        fs::path song_folder = data_root / "fumen" / listing.id;
        std::error_code ec;
        if (!fs::is_directory(song_folder, ec)) {
            continue;
        }
        enqueue_inline_back_every_ten(songs_added, data_root);
        auto box = make_song_box(song_folder, def, SongParser(song_folder));
        // The file's own order is the game's order.
        box->preserve_order = true;
        box->fade_in(266);
        enqueue_inline_box(std::move(box));
        songs_added++;
    }
    return true;
#else
    (void)path;
    (void)box_def;
    return false;
#endif
}

bool Navigator::is_gen4_song_folder(const fs::path& path) {
#ifdef SUPPORT_FUMEN
    std::error_code ec;
    if (!fs::is_directory(path, ec)) {
        return false;
    }

    std::string id = path.filename().string();
    static const char* suffixes[] = {"_e", "_n", "_h", "_m", "_x"};
    bool has_chart = false;
    for (const char* suffix : suffixes) {
        if (fs::exists(path / (id + suffix + ".bin"), ec)) {
            has_chart = true;
            break;
        }
    }
    if (!has_chart) {
        return false;
    }

    return gen4::library_for(path) != nullptr;
#else
    (void)path;
    return false;
#endif
}

bool Navigator::is_osu_song_folder(const fs::path& path) {
    if (auto index = library_index()) {
        if (const LibraryIndex::Dir* listing = index->find(path)) {
            try {
                for (const LibraryIndex::Entry& e : listing->entries) {
                    if (!name_has_ext(e.name, L".osu")) {
                        continue;
                    }
                    if (e.is_reparse ? fs::is_regular_file(path / e.name) : !e.is_dir) {
                        return true;
                    }
                }
            } catch (...) {
            }
            return false;
        }
    }
    if (!fs::is_directory(path)) {
        return false;
    }
    try {
        for (const auto& entry : fs::directory_iterator(path)) {
            if (fs::is_regular_file(entry.path()) && entry.path().extension() == ".osu") {
                return true;
            }
        }
    } catch (...) {
    }
    return false;
}

bool Navigator::is_song(BaseBox* item) {
    // What the box is, not what its path looks like.
    return dynamic_cast<SongBox*>(item) != nullptr;
}

BaseBox* Navigator::get_current_item() {
    if (items.empty()) {
        return nullptr;
    }
    return items[open_index].get();
}

void Navigator::set_positions(bool init, float duration) {
    float num_boxes = items.size();
    for (int i = 0; i < num_boxes; i++) {
        float offset = i - open_index;
        if (offset > num_boxes / 2) {
            offset -= num_boxes;
        } else if (offset < -num_boxes / 2) {
            offset += num_boxes;
        }

        float position;
        if (vertical_gallery) {
            const auto [base_spacing, expand_gap] = row_pitch();
            float center_y = 360 * tex.screen_scale;
            float fixed_x = 640 * tex.screen_scale;
            float horizontal_spacing = 30 * tex.screen_scale;
            if (const SkinInfo* curve = tex.get_skin_config(SC::SONG_SELECT_ROW_CURVE); curve && curve->x > 0) {
                horizontal_spacing = curve->x;
            }
            position = center_y + offset * base_spacing;
            if (offset > 0) {
                position += expand_gap;
            } else if (offset < 0) {
                position -= expand_gap;
            }
            items[i]->vertical = true;
            float new_cross = fixed_x + offset * horizontal_spacing;
            if (init || std::abs(position - items[i]->position) >= tex.screen_height) {
                items[i]->set_position(position);
                items[i]->snap_cross(new_cross);
            } else {
                items[i]->glide_cross(new_cross);
                items[i]->move_box(position, duration);
            }
        } else {
            position = slot_x(offset);
            items[i]->vertical = false;
            if (init || std::abs(position - items[i]->position) >= tex.screen_width) {
                items[i]->set_position(position);
            } else if (zdon_ai_sort_view_on()) {
                // the arcade AI select's list (zdon_ai_arcade_flow): the strips slide 8 frames at 60 fps, quadratic
                // ease-out
                items[i]->move_box(position, 8000.0f / 60.0f, 0.0f, EaseType::Quadratic);
            } else {
                items[i]->move_box(position, duration);
            }
        }
    }
}

bool Navigator::next_uncleared_in_listing(int difficulty) {
    if (items.empty() || inline_streaming || is_processing || difficulty < static_cast<int>(Difficulty::EASY) ||
        difficulty > static_cast<int>(Difficulty::URA)) {
        return false;
    }
    const int n = static_cast<int>(items.size());
    for (int step = 1; step < n; step++) {
        const auto* song = dynamic_cast<const SongBox*>(items[static_cast<size_t>((open_index + step) % n)].get());
        if (song == nullptr) {
            continue;
        }
        const auto info = song->course_info(difficulty);
        if (!info.has_course || info.crown != static_cast<int>(Crown::NONE)) {
            continue;
        }
        navigate(step, true);
        return true;
    }
    return false;
}

bool Navigator::random_song_in_listing() {
    if (items.empty() || inline_streaming || is_processing) {
        return false;
    }
    std::vector<int> candidates;
    for (int i = 0; i < std::ssize(items); i++) {
        if (i != open_index && is_song(items[static_cast<size_t>(i)].get())) {
            candidates.push_back(i);
        }
    }
    if (candidates.empty()) {
        return false;
    }
    static std::mt19937 rng(std::random_device{}());
    const int target = candidates[std::uniform_int_distribution<size_t>(0, candidates.size() - 1)(rng)];
    navigate(target - open_index, true);
    return true;
}

std::pair<float, float> Navigator::row_pitch() const {
    float spacing = 120 * tex.screen_scale;
    float gap = 90 * tex.screen_scale;
    if (const SkinInfo* pitch = tex.get_skin_config(SC::SONG_SELECT_ROW_PITCH)) {
        if (pitch->x > 0) {
            spacing = pitch->x;
        }
        if (pitch->y > 0) {
            gap = pitch->y;
        }
    }
    return {spacing, gap};
}

float Navigator::box_pitch() const {
    return vertical_gallery ? row_pitch().first : 100 * tex.screen_scale;
}

std::optional<int> Navigator::offset_at(Vec2 world) const {
    if (items.empty()) {
        return std::nullopt;
    }
    int offset = 0;
    if (vertical_gallery) {
        const auto [spacing, gap] = row_pitch();
        const float d = world.y - (360 * tex.screen_scale);
        const float beyond = std::abs(d) - gap - (spacing / 2.0f);
        if (beyond > 0) {
            offset = (1 + static_cast<int>(beyond / spacing)) * (d < 0 ? -1 : 1);
        }
    } else {
        const TextureObject* band = tex.get_texture("box/folder_texture_left");
        if (band && (world.y < band->y[0] || world.y >= band->y[0] + band->y2[0])) {
            return std::nullopt;
        }
        const float pitch = box_pitch();
        const float open_left = slot_x(-1) + pitch;
        const float open_right = slot_x(1);
        if (world.x < open_left) {
            offset = static_cast<int>(std::floor((world.x - open_left) / pitch));
        } else if (world.x >= open_right) {
            offset = static_cast<int>((world.x - open_right) / pitch) + 1;
        }
    }
    // a short list leaves the far slots empty
    const auto count = static_cast<int>(items.size());
    for (int i = 0; i < count; i++) {
        float slot = static_cast<float>(i - open_index);
        if (slot > static_cast<float>(count) / 2) {
            slot -= static_cast<float>(count);
        } else if (slot < -static_cast<float>(count) / 2) {
            slot += static_cast<float>(count);
        }
        if (static_cast<int>(slot) == offset) {
            return offset;
        }
    }
    return std::nullopt;
}

float Navigator::slot_x(float offset) const {
    const float scale = tex.screen_scale;
    float position = (444 * scale) + (offset * 100 * scale);
    if (offset > 0) {
        position += 300 * scale;
    } else if (offset == 0) {
        position += 150 * scale;
    }
    return position;
}

void Navigator::navigate(int delta, bool snap) {
    if (items.empty() || inline_streaming) {
        return;
    }
    emit_wheel_event(WHEEL_EVENT_CURSOR_MOVE);
    items[open_index]->close_box();
    last_bg_genre_index = bg_genre_index;
    bg_genre_pending = false;
    open_index = ((open_index + delta) % static_cast<int>(items.size()) + static_cast<int>(items.size())) %
                 static_cast<int>(items.size());
    bg_genre_index = items[open_index]->genre_index;
    set_positions(snap, 166);
    items[open_index]->expand_box();
    background_fade_change->start();
}

bool Navigator::snap_to_genre(GenreIndex genre) {
    if (items.empty() || inline_streaming) {
        return false;
    }
    int target = -1;
    for (int i = 0; i < static_cast<int>(items.size()); i++) {
        if (is_directory(items[i].get()) && items[i]->genre_index == genre) {
            target = i;
            break;
        }
    }
    if (target < 0) {
        return false;
    }
    if (target != open_index) {
        navigate(target - open_index, true);
    }
    last_bg_genre_index = bg_genre_index;
    for (auto& box : items) {
        box->fade->fast_forward();
        box->open_fade->fast_forward();
        box->open_anim->fast_forward();
    }
    items[open_index]->snap_open_pending = true;
    zdon_ai_instant_open_arm();
    background_fade_change->fast_forward();
    return true;
}

void Navigator::move_left() {
    navigate(-1, false);
}
void Navigator::move_right() {
    navigate(+1, false);
}
void Navigator::skip_left() {
    navigate(-10, true);
}
void Navigator::skip_right() {
    navigate(+10, true);
}

void Navigator::enter_current_box() {
    if (!items.empty()) {
        items[open_index]->enter_box();
    }
}

void Navigator::enter_diff_select(bool defer_box, bool fade_in_place) {
    if (!defer_box) {
        items[open_index]->enter_box();
    }
    if (fade_in_place) {
        for (auto& box : items) {
            box->fade_out(300.0f);
        }
        if (auto* song = dynamic_cast<SongBox*>(items[open_index].get())) {
            song->prestart_diff_fade(); // the course board's timing does not change with the deferred enter_box
        }
        if (genre_bg.has_value()) {
            genre_bg->fade_out();
        }
        return;
    }
    const float duration = 800;
    const float distance = (150 * tex.screen_scale);
    // the list runs down the screen in the vertical gallery, across it otherwise
    const float axis = vertical_gallery ? tex.screen_height : tex.screen_width;
    const float mid = vertical_gallery ? 360 * tex.screen_scale : 594 * tex.screen_scale;
    for (int i = 0; i < items.size(); i++) {
        std::unique_ptr<BaseBox>& box = items[i];
        bool on_screen = box->position > -100 && box->position < axis + 100;
        if (on_screen && i != open_index) {
            if (box->position < mid) {
                box->move_box(-distance, duration);
            } else {
                box->move_box(axis + distance, duration);
            }
            box->fade_out();
        }
    }
    if (genre_bg.has_value()) {
        genre_bg->fade_out();
    }
}

void Navigator::exit_diff_select() {
    emit_wheel_event(WHEEL_EVENT_COURSE_BACK);
    items[open_index]->exit_box();
    set_positions(false, 500);
    for (auto& box : items) {
        box->fade_in(166);
    }
    if (genre_bg.has_value()) {
        genre_bg->fade_in();
    }
}

void Navigator::update(double current_ms) {
    if (genre_bg.has_value()) {
        genre_bg->update(current_ms);
    }
    if (inline_state && inline_state->fading_out && pending_inline_folder) {
        pending_inline_folder->update(current_ms);
    }
    background_move->update(current_ms);
    background_fade_change->update(current_ms);

    flush_pending_boxes();

    if (open_start_folder && !is_processing) {
        if (!items.empty() && items[open_index]->path == *open_start_folder) {
            load_current_directory(*open_start_folder);
        }
        open_start_folder.reset();
    }

    if (bg_genre_pending && !items.empty()) {
        GenreIndex cursor_genre = items[open_index]->genre_index;
        if (cursor_genre != bg_genre_index) {
            bg_genre_index = cursor_genre;
            last_bg_genre_index = cursor_genre;
        }
        if (loading_complete) {
            bg_genre_pending = false;
        }
    }

    if (pending_inline_path) {
        if (genre_bg.has_value() && genre_bg->popped_in() && !awaiting_diff_sort) {
            inline_state->saved_folder_box =
                std::unique_ptr<FolderBox>(static_cast<FolderBox*>(items[open_index].release()));
            setup_back_box(*pending_inline_path, false);

            join_loader();
            loading_complete = false;
            is_inline = true;
#ifndef __EMSCRIPTEN__
            loader_thread =
                std::thread(&Navigator::load_songs_inline_async, this, *pending_inline_path, pending_inline_box_def);
            fixed_step::join(loader_thread);
#else
            load_songs_inline_async(*pending_inline_path, pending_inline_box_def);
#endif
            inline_state->songs_count = 0; // will be updated as boxes arrive
            genre_bg_start = genre_bg_end = open_index;
            pending_inline_path.reset();
            is_processing = false; // flush_pending_boxes will finalise
            inline_streaming = true;
        }
    }

    if (inline_state && inline_state->fading_out) {
        InlineState& state = *inline_state;
        int end = state.first_song_index + state.songs_count;

        bool all_done = true;
        for (int i = state.first_song_index; i < end; i++) {
            if (!items[i]->fade->is_finished || (genre_bg && !genre_bg->shut())) {
                all_done = false;
                break;
            }
        }

        if (all_done && swap_is_early(WHEEL_EVENT_CLOSE_BEGIN)) {
            all_done = false;
        }

        if (all_done) {
            pending_inline_folder = nullptr;
            items.erase(items.begin() + state.first_song_index, items.begin() + end);
            items.erase(items.begin() + state.folder_index);
            items.insert(items.begin() + state.folder_index, std::move(state.saved_folder_box));
            open_index = state.folder_index;
            inline_state.reset();
            for (const auto& box : items) {
                box->ride = 0.0f;
            }
            set_positions(true, 0);
            items[open_index]->exit_box();
            items[open_index]->fade_in(100, 400);
            if (genre_bg) {
                genre_bg->pop_out();
            } else {
                is_processing = false;
            }
            emit_wheel_event(WHEEL_EVENT_CLOSE_SWAP);
        }
    }

    if (genre_bg && genre_bg->is_done()) {
        genre_bg.reset();
        is_processing = false;
    }
    if (genre_bg && inline_state && !pending_inline_path && !inline_streaming && !inline_state->fading_out &&
        genre_bg->phase() == GenreBG::Phase::WAIT) {
        genre_bg->open_sides();
        is_processing = true;
    }

    SongBox::service_bgm_resume(current_ms);

    for (auto& box : items) {
        bool on_screen = vertical_gallery ? (box->position > -100 && box->position < tex.screen_height + 100)
                                          : (box->box_x() > -100 && box->box_x() < tex.screen_width + 100);
        if (on_screen && !box->text_loaded) {
            box->load_text();
        }
        box->update(current_ms);
    }
    update_genre_edges();
}

void Navigator::update_genre_edges() {
    if (!genre_bg) {
        return;
    }
    const float pitch = 100 * tex.screen_scale;
    GenreBG::Span closed{445 * tex.screen_scale, 834 * tex.screen_scale};
    if (const SkinInfo* boundary = tex.get_skin_config(SC::SONG_SELECT_GENRE_BG_BOUNDARY);
        boundary && boundary->width > 0) {
        closed = {boundary->x, boundary->width};
    }
    const float center = (closed.left + closed.right) / 2.0f;
    GenreBG::Span live = closed;
    const auto count = static_cast<int>(items.size());
    const bool listed = !vertical_gallery && inline_state && !pending_inline_path && genre_bg_start <= genre_bg_end &&
                        genre_bg_end < count;
    genre_bg->wrap.reset();
    genre_bg->show_tab = true;
    if (listed) {
        constexpr float UNBOUNDED = 1e6f;
        const auto slot_offset = [&](int i) {
            int offset = i - open_index;
            if (offset > count / 2.0f) {
                offset -= count;
            } else if (offset < -count / 2.0f) {
                offset += count;
            }
            return offset;
        };
        const int first_offset = slot_offset(genre_bg_start);
        const int last_offset = slot_offset(genre_bg_end);
        const float first = items[genre_bg_start]->position + closed.left - pitch - slot_x(-1);
        const float last = items[genre_bg_end]->position + closed.right + pitch - slot_x(1);
        const bool inside = open_index >= genre_bg_start && open_index <= genre_bg_end;
        if (inside) {
            live.left = first_offset > 0 ? -UNBOUNDED : std::min(first, closed.left);
            live.right = last_offset < 0 ? UNBOUNDED : std::max(last, closed.right);
            if (genre_bg->phase() == GenreBG::Phase::OPEN) {
                if (first_offset > 0) {
                    genre_bg->wrap = GenreBG::Span{first, tex.screen_width + pitch};
                } else if (last_offset < 0) {
                    genre_bg->wrap = GenreBG::Span{-pitch, last};
                }
            }
        } else {
            genre_bg->show_tab = false;
            const bool around = first_offset > last_offset;
            live = {first, around ? UNBOUNDED : last};
            if (around) {
                genre_bg->wrap = GenreBG::Span{-pitch, last};
            }
        }
    }

    const GenreBG::Phase before = genre_bg->phase();
    genre_bg->set_edges(closed, live, pitch);
    const GenreBG::Phase phase = genre_bg->phase();
    if (before == GenreBG::Phase::OPENING && phase == GenreBG::Phase::OPEN) {
        is_processing = false;
        if (listed && !vertical_gallery) {
            enter_wrapped_boxes(center);
        }
    }

    if (vertical_gallery) {
        return;
    }

    const bool riding = listed && phase != GenreBG::Phase::OPEN;
    const int closed_count = count - (genre_bg_end - genre_bg_start);
    for (int i = 0; i < count; i++) {
        BaseBox& box = *items[i];
        box.ride = 0.0f;
        if (!riding || (i >= genre_bg_start && i <= genre_bg_end)) {
            continue;
        }
        const int after = ((i - genre_bg_end) % count + count) % count;
        const float offset = after <= closed_count / 2.0f ? after : after - closed_count;
        const float shift = offset > 0 ? genre_bg->right - closed.right : genre_bg->left - closed.left;
        box.ride = slot_x(offset) + shift - box.position;
    }
}

void Navigator::enter_wrapped_boxes(float center) {
    const auto count = static_cast<int>(items.size());
    const int closed_count = count - (genre_bg_end - genre_bg_start);
    set_positions(true, 0);
    for (int i = 0; i < count; i++) {
        if (i >= genre_bg_start && i <= genre_bg_end) {
            continue;
        }
        const int after = ((i - genre_bg_end) % count + count) % count;
        BaseBox& box = *items[i];
        const float to = box.position;
        if (after <= closed_count / 2.0f && to < center && to > -100 * tex.screen_scale) {
            box.set_position(-100 * tex.screen_scale);
            box.move_box(to, 500, 0, EaseType::Quadratic);
        }
    }
}

float Navigator::get_diff_fade_in() {
    SongBox* current_item = dynamic_cast<SongBox*>(get_current_item());
    if (!current_item || !current_item->diff_fade_in) {
        return 0.0f;
    }
    return current_item->diff_fade_in->attribute;
}

void Navigator::draw_background() {
    if (script && script->draw_background(this)) {
        return;
    }
    if (!t_background) { // a skin without box/background art
        return;
    }

    int width = t_background->width;
    for (int i = 0; i < width * 4; i += width) {
        tex.draw_texture(t_background, {.frame = static_cast<int>(last_bg_genre_index),
                                        .x = static_cast<float>(i - background_move->attribute)});
        tex.draw_texture(t_background, {.frame = static_cast<int>(bg_genre_index),
                                        .x = static_cast<float>(i - background_move->attribute),
                                        .fade = 1.0f - background_fade_change->attribute});
    }
}

void Navigator::draw() {
    if (!vertical_gallery && genre_bg.has_value() && script) {
        script->draw_genre_bg(&genre_bg.value());
    }
    for (auto& box : items) {
        bool on_screen = vertical_gallery ? (box->position > -100 && box->position < tex.screen_height + 100)
                                          : (box->box_x() > -100 && box->box_x() < tex.screen_width + 100);
        if (on_screen && script) {
            script->draw_box(box.get());
        }
    }
}

void Navigator::add_click_areas() const {
    const int n = static_cast<int>(items.size());
    if (n == 0 || open_index >= items.size()) {
        return;
    }
    const float s = tex.screen_scale;
    // the list's pitch and the open row's extra gap, as set_positions places them
    float pitch = vertical_gallery ? 120 * s : 100 * s;
    float gap = vertical_gallery ? 90 * s : 0.0f;
    if (vertical_gallery) {
        if (const SkinInfo* p = tex.get_skin_config(SC::SONG_SELECT_ROW_PITCH)) {
            if (p->x > 0) {
                pitch = p->x;
            }
            if (p->y > 0) {
                gap = p->y;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        int offset = i - static_cast<int>(open_index);
        if (offset > n / 2) {
            offset -= n;
        } else if (offset < -n / 2) {
            offset += n;
        }
        const auto& box = items[i];
        if (vertical_gallery) {
            if (box->position < -100 || box->position > tex.screen_height + 100) {
                continue;
            }
            // a row: its pitch tall about its y (the open one also takes the gaps around it), about its x (the centre)
            const float h = offset == 0 ? pitch + 2 * gap : pitch;
            zdon_mouse::add_area(box->box_x() - 330 * s, box->position - h / 2, 660 * s, h, offset, i);
        } else {
            if (box->position < -100 || box->position > tex.screen_width + 100) {
                continue;
            }
            // a column: the open one from its left to its right bound, the others a pitch wide
            const float x0 = offset == 0 ? box->left_bound : box->position;
            const float w = offset == 0 ? std::max(box->right_bound - box->left_bound, pitch) : pitch;
            zdon_mouse::add_area(x0, 90 * s, w, 540 * s, offset, i);
        }
    }
}

void Navigator::draw_diff_select_bg() {
    if (open_index >= items.size()) {
        return;
    }
    BaseBox* box = items[open_index].get();
    if (script) {
        script->draw_box_bg(box);
    }
}

void Navigator::draw_score_history() {
    if (open_index < items.size() && dynamic_cast<SongBox*>(items[open_index].get()) != nullptr) {
        items[open_index]->draw_score_history();
    }
}

// Course clear counts for the difficulty sort. The levels come from the startup scan (song_levels),
// not a parse of every chart on disk: song select waits for this before it opens the difficulty
// sort and before it hands over to the next screen.
Statistics Navigator::get_statistics(const std::vector<fs::path>& paths, const std::stop_token& stop,
                                     const std::set<fs::path>* scope) {
    std::atomic<bool> cancelled{stop.stop_requested()};
    std::stop_callback cancellation(stop, [&cancelled] { cancelled = true; });
    Statistics stats;

    for (int course = 0; course <= 4; course++) {
        for (int level = 1; level <= 10; level++) {
            stats[course][level] = CourseStats{};
        }
    }

    // Wait for the scan without joining its thread (the loader does that)
    while (!song_files_ready.load() && !abort_loading.load() && !stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    if (!song_files_ready.load() || stop.stop_requested()) {
        return stats;
    }

    // Songs below a folder of a root (not files directly in it), as the directory walk counted them
    std::vector<std::pair<fs::path, std::array<int, 5>>> songs;
    for (const fs::path& path : paths) {
        for (const auto& [path_str, levels] : song_levels) {
            fs::path song_path(path_str);
            fs::path rel = song_path.lexically_relative(path);
            if (rel.empty() || *rel.begin() == ".." || std::distance(rel.begin(), rel.end()) < 2) {
                continue;
            }
            if (!scope || scope->contains(song_path)) {
                std::array<int, 5> course_levels{};
                std::ranges::copy_n(levels.begin(), 5, course_levels.begin());
                songs.emplace_back(song_path, course_levels);
            }
        }
    }

    for (const auto& [song_path, levels] : songs) {
        if (stop.stop_requested()) {
            break;
        }
        const auto& hashes = scores_manager.get_hashes(song_path);
        for (int course = 0; course <= 4; course++) {
            int level = levels[course];
            if (level < 1 || level > 10) {
                continue;
            }

            CourseStats& cs = stats[course][level];
            cs.total++;

            const std::string& hash = hashes[course];
            if (hash.empty()) {
                continue;
            }

            if (auto score = scores_manager.get_score(hash, course, scores_manager.player_1)) {
                cs.full_combos += score->crown >= Crown::FC;
                cs.clears += score->crown >= Crown::CLEAR;
            }
            if (scores_manager.player_2.empty()) {
                continue;
            }
            if (auto score = scores_manager.get_score(hash, course, scores_manager.player_2)) {
                cs.full_combos_2p += score->crown >= Crown::FC;
                cs.clears_2p += score->crown >= Crown::CLEAR;
            }
        }
    }
    return stats;
}

void replace_all(std::string& str, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return;
    }

    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

std::string normalize_title(std::string s) {
    replace_all(s, "-New Audio-", "");
    replace_all(s, "-新曲-", "");
    replace_all(s, "-Old Audio-", "");
    replace_all(s, "-旧曲-", "");

    // Keep UTF-8 (bytes >= 0x80) so a Japanese / Chinese title survives; only ASCII
    // punctuation and spaces are dropped. Stripping every non-alnum byte turned every
    // all-CJK title into "", so any two such songs compared equal.
    std::erase_if(s, [](unsigned char c) { return c < 0x80 && !std::isalnum(c); });

    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return s;
}

std::optional<fs::path> Navigator::find_song_by_title(const std::string& title, const std::string& subtitle) {
    std::string norm_title = normalize_title(title);
    std::string norm_subtitle = normalize_title(subtitle);
    if (norm_title.empty()) {
        return std::nullopt; // nothing to match on
    }

    std::scoped_lock lock(title_index_mutex);
    if (title_index_size != song_files.size()) {
        // First match in song_files order wins, as with the linear scans this replaced
        title_subtitle_index.clear();
        title_only_index.clear();
        normalized_titles.clear();
        normalized_titles.reserve(song_files.size());
        for (auto& [key, path] : song_files) {
            std::string t = normalize_title(key.first);
            title_subtitle_index.emplace(t + '\x1f' + normalize_title(key.second), path);
            title_only_index.emplace(t, path);
            normalized_titles.emplace_back(std::move(t), path);
        }
        title_index_size = song_files.size();
    }

    if (auto it = title_subtitle_index.find(norm_title + '\x1f' + norm_subtitle); it != title_subtitle_index.end()) {
        return it->second;
    }

    // Fallback: title only
    if (auto it = title_only_index.find(norm_title); it != title_only_index.end()) {
        return it->second;
    }

    for (auto& [indexed_title, path] : normalized_titles) {

        // Check if norm_title is inside indexed_title or vice-versa
        if (indexed_title.contains(norm_title) || norm_title.contains(indexed_title)) {

            // Safety: Don't match tiny fragments like "a" or "the"
            if (norm_title.length() < 3 && indexed_title != norm_title) {
                continue;
            }

            return path;
        }
    }

    return std::nullopt;
}

Navigator navigator;

// ---------------------------------------------------------------- Z-DON song search panel (scenes/song_select.cpp)

// the top genre folder a song sits in (its box.def name), cached; -1 / "" when it has none
std::pair<int, std::string> Navigator::zdon_genre_of(const fs::path& song_path) {
    const std::string key = song_path.string();
    if (auto it = zdon_genre_cache.find(key); it != zdon_genre_cache.end()) {
        return it->second;
    }
    std::pair<int, std::string> g{-1, ""};
    fs::path cur = song_path;
    while (true) {
        fs::path folder = find_box_def_folder(cur);
        if (folder.empty()) {
            break;
        }
        const BoxDef def = parse_box_def(folder);
        g = {static_cast<int>(def.genre_index), def.name};
        if (std::ranges::contains(root_paths, folder.parent_path())) {
            break;
        }
        cur = folder;
    }
    zdon_genre_cache.emplace(key, g);
    return g;
}

// the genres of the library, in first-seen order
std::vector<std::string> Navigator::zdon_genres() {
    std::vector<std::string> out;
    if (!song_files_ready) {
        return out;
    }
    for (const auto& [key, song_path] : song_files) {
        const std::string name = zdon_genre_of(song_path).second;
        if (!name.empty() && !std::ranges::contains(out, name)) {
            out.push_back(name);
        }
    }
    return out;
}

std::vector<fs::path> Navigator::zdon_search(const std::string& query, const std::string& genre, int course, int level,
                                             size_t limit, size_t& total) {
    std::vector<fs::path> hits;
    total = 0;
    if (!song_files_ready) {
        return hits;
    }
    const std::string q = search_fold(query);
    for (const auto& [key, song_path] : song_files) {
        const std::string p = song_path.string();
        if (!q.empty()) {
            auto st = song_search_text.find(p);
            const std::string& text = (st != song_search_text.end()) ? st->second : search_fold(key.first);
            if (!text.contains(q)) {
                continue;
            }
        }
        if (course >= 0 || level > 0) {
            auto lv = song_levels.find(p);
            if (lv == song_levels.end()) {
                continue;
            }
            bool ok = false;
            for (int c = 0; c < 5 && !ok; ++c) {
                ok = (course < 0 || c == course) && lv->second[c] >= 0 && (level <= 0 || lv->second[c] == level);
            }
            if (!ok) {
                continue;
            }
        }
        if (!genre.empty() && zdon_genre_of(song_path).second != genre) {
            continue;
        }
        total++;
        if (hits.size() < limit) {
            hits.push_back(song_path);
        }
    }
    return hits;
}

std::array<int, 5> Navigator::zdon_levels_of(const fs::path& song_path) const {
    auto it = song_levels.find(song_path.string());
    if (it == song_levels.end()) {
        return {-1, -1, -1, -1, -1};
    }
    return it->second;
}
