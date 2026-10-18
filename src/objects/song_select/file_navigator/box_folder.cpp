#include "box_folder.h"
#ifdef SUPPORT_FUMEN
#include "../../../libs/optional/gen3.h"
#include "../../../libs/optional/gen4.h"
#endif
#include <deque>
#include <mutex>

#include "../../../libs/audio.h"
#include "../../../libs/filesystem.h"
#include "../../../libs/scores.h"
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
        std::lock_guard<std::mutex> lock(scan_cache_mutex);
        scan_cache[path] = std::move(scan);
    }

    void scan_folder_now(const fs::path& path) {
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

                if (disqualified.count(diff)) {
                    continue;
                }

                auto [it, inserted] = out_crown.try_emplace(diff, score->crown);
                if (!inserted) {
                    it->second = std::min(it->second, score->crown);
                }
            }
        };
        auto update_both = [&](const fs::path& file_path) {
            const auto hashes = scores_manager.get_hashes(file_path);
            update_crown(hashes, player_1_id, crown, disqualified_p1);
            update_crown(hashes, player_2_id, crown_p2, disqualified_p2);
        };

        // Errors are stepped over rather than thrown: one unreadable entry deep in
        // a tree should not take the folder box down with it.
        std::error_code scan_ec;
        auto scan = fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, scan_ec);
        while (scan != fs::end(scan)) {
            const fs::directory_entry& entry = *scan;
            if (entry.path().filename() == "song_list.txt") {
                auto entries = read_song_list(entry.path());
                tja_count += (int)entries.size();
                for (const auto& e : entries) {
                    if (auto found = scores_manager.get_path_by_hash(e.hash)) {
                        update_both(*found);
                    }
                }
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

    void load_box_texture(std::optional<ray::Texture>& out, const fs::path& file) {
        out = ray::LoadTexture(file.string().c_str());
        ray::GenTextureMipmaps(&out.value());
        ray::SetTextureFilter(out.value(), ray::TEXTURE_FILTER_TRILINEAR);
    }
} // namespace

void FolderBox::invalidate_scan_cache() {
    std::lock_guard<std::mutex> lock(scan_cache_mutex);
    scan_cache.clear();
}

FolderBox::FolderBox(const fs::path& path, const BoxDef& box_def) : BaseBox(path, box_def), tja_count(0) {
    this->text_name = box_def.name;
    enter_fade = std::make_unique<FadeAnimation>(166);
    refresh_scores();
}

void FolderBox::refresh_scores() {
    {
        std::lock_guard<std::mutex> lock(scan_cache_mutex);
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
    std::lock_guard<std::mutex> lock(deferred_mutex);
    deferred_scans.push_back(path);
}

void FolderBox::run_deferred_scans(std::atomic<bool>& abort_flag) {
    for (;;) {
        if (abort_flag) {
            return; // leave the rest queued for the next load
        }
        fs::path next;
        {
            std::lock_guard<std::mutex> lock(deferred_mutex);
            if (deferred_scans.empty()) {
                return;
            }
            next = std::move(deferred_scans.front());
            deferred_scans.pop_front();
        }
        {
            std::lock_guard<std::mutex> lock(scan_cache_mutex);
            if (scan_cache.count(next)) {
                continue;
            }
        }
        scan_folder_now(next);
    }
}

FolderBox::~FolderBox() {
    if (box_texture.has_value()) {
        ray::UnloadTexture(box_texture.value());
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
            load_box_texture(box_texture, it->path());
        }
    } else if (fs::exists(fs::path(path / "box.png")) && !box_texture.has_value()) {
        load_box_texture(box_texture, path / "box.png");
    }
    text_loaded = true;
}

void FolderBox::update(double current_time) {
    if (scan_pending) {
        std::lock_guard<std::mutex> lock(scan_cache_mutex);
        auto it = scan_cache.find(path);
        if (it != scan_cache.end()) {
            crown = it->second.crown;
            crown_p2 = it->second.crown_p2;
            tja_count = it->second.tja_count;
            scan_pending = false;
        }
    }

    bool is_open_prev = yellow_box_opened;
    enter_fade->update(current_time);
    BaseBox::update(current_time);

    if (!is_open_prev && yellow_box_opened) {
        if (!audio.is_sound_playing("voice_enter")) {
            audio.play_sound("genre_voice_" + std::to_string((int)genre_index), VolumePreset::VOICE);
            genre_voice_started = true;
        }
    } else if (!yellow_box_opened && genre_voice_started) {
        audio.stop_sound("genre_voice_" + std::to_string((int)genre_index));
        genre_voice_started = false;
    }
}

void FolderBox::enter_box() {
    entered = true;
    enter_fade->start();
    if (genre_voice_started) {
        audio.stop_sound("genre_voice_" + std::to_string((int)genre_index));
    }
}

void FolderBox::exit_box() {
    entered = false;
    enter_fade->reset();
}
