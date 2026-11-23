#include "box_folder.h"
#ifdef SUPPORT_FUMEN
#include "../../../libs/optional/gen3.h"
#include "../../../libs/optional/gen4.h"
#endif
#include <algorithm>
#include <deque>
#include <mutex>

#include "../../../libs/audio/audio.h"
#include "../../../libs/file/filesystem.h"
#include "../../../libs/file/scores.h"
#include "../../../libs/library_index.h"
#include "navigator.h"

namespace {

    struct FolderScan {
        std::map<int, Crown> crown;
        std::map<int, Crown> crown_p2;
        int tja_count;
    };
    std::mutex scan_cache_mutex;
    std::map<fs::path, FolderScan> scan_cache;

    std::mutex deferred_mutex;
    std::deque<fs::path> deferred_scans;

    void store_scan(const fs::path& path, FolderScan scan) {
        std::scoped_lock lock(scan_cache_mutex);
        scan_cache[path] = std::move(scan);
    }

    // The entries the recursive_directory_iterator in scan_folder_now visits, from the library index:
    // every song_list.txt and every .tja / .osu (by name, as that loop checks them) under `dir`,
    // links followed like that iterator does. False when a folder of the tree is not in the index.
    bool scan_index_tree(const LibraryIndex& index, const fs::path& dir, const LibraryIndex::Dir& listing,
                         std::vector<const LibraryIndex::Dir*>& stack, std::vector<fs::path>& song_lists,
                         std::vector<fs::path>& charts) {
        bool has_song_list = false;
        for (const LibraryIndex::Entry& e : listing.entries) {
            fs::path p = dir / e.name;
            if (e.name.starts_with(L"song_list") && is_song_list_file(p)) { // upstream #381: account lists
                has_song_list = true;
                song_lists.push_back(p);
            } else if (name_has_ext(e.name, L".tja") || name_has_ext(e.name, L".osu")) {
                charts.push_back(p);
            }
            bool is_dir = e.is_dir;
            if (e.is_reparse) {
                std::error_code ec;
                is_dir = fs::is_directory(p, ec);
            }
            if (!is_dir) {
                continue;
            }
            const LibraryIndex::Dir* sub = index.child(e);
            if (!sub || std::ranges::find(stack, sub) != stack.end()) {
                return false; // not listed by the walk, or a link back to a folder above
            }
            stack.push_back(sub);
            const bool ok = scan_index_tree(index, p, *sub, stack, song_lists, charts);
            stack.pop_back();
            if (!ok) {
                return false;
            }
        }
        // a song_list.txt written since the walk (favorites / recently played)
        if (!has_song_list && index.song_list_written(dir)) {
            std::error_code ec;
            if (const fs::path file = song_list_file(dir); fs::exists(file, ec)) {
                song_lists.push_back(file);
            }
        }
        return true;
    }

    // `visited` (YATAIDON_LIBRARY_DUMP): every chart whose scores were read, in scan order
    void scan_folder_now(const fs::path& path, std::vector<std::string>* visited = nullptr) {
        std::map<int, Crown> crown;
        std::map<int, Crown> crown_p2;
        int tja_count = 0;
        std::set<int> disqualified_p1;
        std::set<int> disqualified_p2;

        const std::string& player_1_id = global_data.config->network.access_code_1;
        const std::string& player_2_id = global_data.config->network.access_code_2;

        auto update_crown = [&](const std::array<std::string, 5>& hashes, const std::string& player_id,
                                std::map<int, Crown>& out_crown, std::set<int>& disqualified) {
            for (int diff = 0; diff < 5; diff++) {
                if (hashes[diff].empty()) {
                    continue;
                }
                auto score = scores_manager.get_score(hashes[diff], diff, player_id);

                if (!score || score->crown == Crown::NONE) {
                    out_crown.erase(diff);
                    disqualified.insert(diff);
                    continue;
                }

                if (disqualified.contains(diff)) {
                    continue;
                }

                auto [it, inserted] = out_crown.try_emplace(diff, score->crown);
                if (!inserted) {
                    it->second = std::min(it->second, score->crown);
                }
            }
        };
        auto update_both = [&](const fs::path& file_path) {
            if (visited) {
                visited->push_back(file_path.string());
            }
            const auto hashes = scores_manager.get_hashes(file_path);
            update_crown(hashes, player_1_id, crown, disqualified_p1);
            update_crown(hashes, player_2_id, crown_p2, disqualified_p2);
        };

        auto add_song_list = [&](const fs::path& song_list) {
            auto entries = read_song_list(song_list);
            tja_count += static_cast<int>(entries.size());
            for (const auto& e : entries) {
                if (auto found = scores_manager.get_path_by_hash(e.hash)) {
                    update_both(*found);
                }
            }
        };

        // The library walk listed the tree already: no disk access (the song_list.txt files aside)
        if (auto index = library_index()) {
            if (const LibraryIndex::Dir* listing = index->find(path)) {
                std::vector<const LibraryIndex::Dir*> stack{listing};
                std::vector<fs::path> song_lists, charts;
                if (scan_index_tree(*index, path, *listing, stack, song_lists, charts)) {
                    for (const fs::path& song_list : song_lists) {
                        add_song_list(song_list);
                    }
                    tja_count += static_cast<int>(charts.size());
                    for (const fs::path& chart : charts) {
                        update_both(chart);
                    }
                    store_scan(path, {crown, crown_p2, tja_count});
                    return;
                }
            }
        }

        // Errors are stepped over rather than thrown: one unreadable entry deep in
        // a tree should not take the folder box down with it.
        std::error_code scan_ec;
        auto scan = fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, scan_ec);
        while (scan != fs::end(scan)) {
            const fs::directory_entry& entry = *scan;
            if (entry.path().filename().native().starts_with(fs::path("song_list").native()) &&
                is_song_list_file(entry.path())) {
                add_song_list(entry.path());
                scan.increment(scan_ec);
                if (scan_ec) {
                    break;
                }
                continue;
            }
            auto ext = entry.path().extension();
            if (ext == ".tja" || ext == ".osu") {
                tja_count++;
                update_both(entry.path());
            }

            scan.increment(scan_ec);
            if (scan_ec) {
                break;
            }
        }

        store_scan(path, {crown, crown_p2, tja_count});
    }
} // namespace

void FolderBox::invalidate_scan_cache() {
    std::scoped_lock lock(scan_cache_mutex);
    scan_cache.clear();
}

void FolderBox::dump_scans(std::ostream& out, const std::vector<fs::path>& scan_paths) {
    auto write = [&out](const fs::path& path, const FolderScan& scan) {
        out << path.string() << " tja_count=" << scan.tja_count << " crown=";
        for (const auto& [diff, c] : scan.crown) {
            out << diff << ':' << static_cast<int>(c) << ',';
        }
        out << " crown_p2=";
        for (const auto& [diff, c] : scan.crown_p2) {
            out << diff << ':' << static_cast<int>(c) << ',';
        }
        out << '\n';
    };
    std::map<fs::path, FolderScan> cached;
    {
        std::scoped_lock lock(scan_cache_mutex);
        cached = scan_cache;
    }
    out << "[cached folder scans]\n";
    for (const auto& [path, scan] : cached) {
        write(path, scan);
    }
    out << "[folder scans]\n";
    for (const fs::path& path : scan_paths) {
        std::vector<std::string> visited;
        scan_folder_now(path, &visited);
        std::ranges::sort(visited);
        uint64_t fnv = 1469598103934665603ull;
        for (const std::string& v : visited) {
            for (unsigned char c : v + '\n') {
                fnv = (fnv ^ c) * 1099511628211ull;
            }
        }
        out << "visited=" << visited.size() << " fnv=" << fnv << ' ';
        std::scoped_lock lock(scan_cache_mutex);
        write(path, scan_cache[path]);
    }
}

FolderBox::FolderBox(const fs::path& path, const BoxDef& box_def) : BaseBox(path, box_def) {
    this->text_name = box_def.name;
    enter_fade = std::make_unique<FadeAnimation>(250);
    refresh_scores();
}

void FolderBox::refresh_scores() {
    {
        std::scoped_lock lock(scan_cache_mutex);
        auto it = scan_cache.find(path);
        if (it != scan_cache.end()) {
            crown = it->second.crown;
            crown_p2 = it->second.crown_p2;
            tja_count = it->second.tja_count;
            return;
        }
    }

    crown.clear();
    crown_p2.clear();
    tja_count = 0;

#ifdef SUPPORT_FUMEN
    if (const gen4::Library* library = gen4::library_for(path)) {
        int genre_no = gen4::genre_of_path(path);
        for (const gen4::OrderEntry& listing : library->order()) {
            if (genre_no < 0 || listing.genre_no == genre_no) {
                tja_count++;
            }
        }
        store_scan(path, {crown, crown_p2, tja_count});
        return;
    }
    if (const gen3::Library* library = gen3::library_for(path)) {
        std::string genre = gen3::genre_of_path(path);
        for (const gen3::SongEntry& e : library->songs()) {
            if (genre.empty() || e.genre == genre) {
                tja_count++;
            }
        }
        store_scan(path, {crown, crown_p2, tja_count});
        return;
    }
#endif

    scan_pending = true;
    std::scoped_lock lock(deferred_mutex);
    deferred_scans.push_back(path);
}

void FolderBox::run_deferred_scans(std::atomic<bool>& abort_flag) {
    for (;;) {
        if (abort_flag) {
            return; // leave the rest queued for the next load
        }
        fs::path next;
        {
            std::scoped_lock lock(deferred_mutex);
            if (deferred_scans.empty()) {
                return;
            }
            next = std::move(deferred_scans.front());
            deferred_scans.pop_front();
        }
        {
            std::scoped_lock lock(scan_cache_mutex);
            if (scan_cache.contains(next)) {
                continue;
            }
        }
        scan_folder_now(next);
    }
}

FolderBox::~FolderBox() {
    if (!custom_voice.empty()) {
        audio.unload_sound(custom_voice);
    }
    if (box_texture.has_value()) {
        unload_texture(box_texture.value());
    }
    if (box_icon.has_value()) {
        unload_texture(box_icon.value());
    }
}

void FolderBox::load_text() {
    BaseBox::load_text();
    if (is_osu_folder) {
        std::error_code dir_ec;
        auto it = fs::directory_iterator(path, dir_ec);
        auto end = fs::directory_iterator();
        while (!dir_ec && it != end && it->path().extension() != ".jpg" && it->path().extension() != ".png") {
            it++;
        }
        if (!dir_ec && it != end) {
            box_texture = load_texture_mipmapped((it->path()).string().c_str());
        }
    } else if (fs::exists(fs::path(path / "box.png")) && !box_texture.has_value()) {
        box_texture = load_texture_mipmapped((path / "box.png").string().c_str());
    }
    if (!is_osu_folder && fs::exists(path / "box_icon.png") && !box_icon.has_value()) {
        box_icon = load_texture_mipmapped((path / "box_icon.png").string().c_str());
    }
    if (!is_osu_folder && custom_voice.empty() && fs::exists(path / "box.ogg")) {
        custom_voice = audio.load_sound(path / "box.ogg", "box_voice_" + path.string());
    }
    text_loaded = true;
}

std::string FolderBox::hover_voice() const {
    if (collection == WAIWAI_MODE_COLLECTION) {
        return "voice_waiwai_open";
    }
    if (collection == WAIWAI_NORMAL_COLLECTION) {
        return "voice_waiwai_exit";
    }
    if (!custom_voice.empty()) {
        return custom_voice;
    }
    return "genre_voice_" + std::to_string(static_cast<int>(genre_index));
}

void FolderBox::update(double current_time) {
    if (scan_pending) {
        std::scoped_lock lock(scan_cache_mutex);
        auto it = scan_cache.find(path);
        if (it != scan_cache.end()) {
            crown = it->second.crown;
            crown_p2 = it->second.crown_p2;
            tja_count = it->second.tja_count;
            scan_pending = false;
        }
    }

    bool is_open_prev = yellow_box_opened;
    if (!enter_fade->settled(current_time)) {
        enter_fade->update(current_time);
    }
    BaseBox::update(current_time);

    if (!is_open_prev && yellow_box_opened) {
        genre_voice_pending = global_data.hold_genre_voice;
        if (!genre_voice_pending && !audio.is_sound_playing("voice_enter")) {
            audio.play_sound(hover_voice(), VolumePreset::VOICE);
            genre_voice_started = true;
        }
    } else if (yellow_box_opened && genre_voice_pending && !global_data.hold_genre_voice) {
        genre_voice_pending = false;
        audio.play_sound(hover_voice(), VolumePreset::VOICE);
        genre_voice_started = true;
    } else if (!yellow_box_opened) {
        genre_voice_pending = false;
        if (genre_voice_started) {
            audio.stop_sound(hover_voice());
            genre_voice_started = false;
        }
    }
}

void FolderBox::enter_box() {
    entered = true;
    enter_fade->start();
    if (genre_voice_started) {
        audio.stop_sound(hover_voice());
    }
}

void FolderBox::exit_box() {
    entered = false;
    enter_fade->reset();
}
