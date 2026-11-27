#include "zdon_forge_songs.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <rapidjson/document.h>
#include <set>
#include <spdlog/spdlog.h>
#include <sstream>
#include <vector>

#include "../global_data.h"
#include "network.h"

namespace fs = std::filesystem;

namespace zdon_forge_songs {

    namespace {
        constexpr int kListTimeoutMs = 20000;
        constexpr int kTjaTimeoutMs = 30000;
        constexpr int kAudioTimeoutMs = 200000; // TaikoForge converts a song's first audio request
        constexpr int kWorkers = 4;
        const char* const kIdFile = ".forge_id";
        const char* const kAudio = "song.ogg";
        const char* const kChart = "song.tja";

        std::string read_file(const fs::path& p) {
            std::ifstream in(p, std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }

        // writes when the content differs; true when it wrote
        bool write_if_changed(const fs::path& p, const std::string& data) {
            std::error_code ec;
            if (fs::is_regular_file(p, ec) && read_file(p) == data) {
                return false;
            }
            const fs::path tmp = p.string() + ".part";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                out.write(data.data(), static_cast<std::streamsize>(data.size()));
                if (!out) {
                    fs::remove(tmp, ec);
                    return false;
                }
            }
            fs::rename(tmp, p, ec);
            if (ec) {
                fs::remove(tmp, ec);
                return false;
            }
            return true;
        }

        // a folder name from a chart title: no characters Windows refuses, no dots or spaces at the ends
        std::string folder_name(std::string title) {
            // TaikoForge's title is often the uploaded audio's file name: no extension in the folder name
            for (const char* ext : {".ogg", ".mp3", ".wav", ".flac", ".m4a", ".opus"}) {
                const std::string e = ext;
                if (title.size() > e.size() && std::equal(e.rbegin(), e.rend(), title.rbegin(), [](char a, char b) {
                        return a == std::tolower(static_cast<unsigned char>(b));
                    })) {
                    title.resize(title.size() - e.size());
                    break;
                }
            }
            std::string out;
            for (unsigned char c : title) {
                if (c < 32 || std::string_view("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string_view::npos) {
                    continue;
                }
                out.push_back(static_cast<char>(c));
            }
            while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
                out.pop_back();
            }
            while (!out.empty() && (out.front() == ' ' || out.front() == '.')) {
                out.erase(out.begin());
            }
            if (out.size() > 60) { // cut on a UTF-8 character boundary
                size_t cut = 60;
                while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
                    --cut;
                }
                out.resize(cut);
            }
            return out.empty() ? std::string("Song") : out;
        }

        // the chart plays the audio beside it: WAVE:song.ogg (as the web version writes it)
        std::string with_local_wave(const std::string& tja) {
            std::string out;
            out.reserve(tja.size() + 16);
            bool replaced = false;
            std::istringstream in(tja);
            std::string line;
            while (std::getline(in, line)) {
                if (!replaced && line.rfind("WAVE:", 0) == 0) {
                    out += "WAVE:";
                    out += kAudio;
                    out += line.ends_with('\r') ? "\r\n" : "\n";
                    replaced = true;
                    continue;
                }
                out += line;
                out += '\n';
            }
            return replaced ? out : std::string("WAVE:") + kAudio + "\n" + tja;
        }

        struct Song {
            std::string id;
            std::string title;
        };
    } // namespace

    fs::path folder() {
        const auto& paths = global_data.config->paths;
        for (const fs::path& p : paths.tja_path) {
            const bool mirror = std::ranges::any_of(paths.song_manifests, [&](const auto& m) { return m.second == p; });
            if (!mirror) {
                return p / "TaikoForge";
            }
        }
        return fs::path("Songs") / "TaikoForge";
    }

    bool sync(const std::string& token, Progress& progress) {
        if (token.empty()) {
            return false;
        }
        const auto [status, body] = network.signed_get_now("/forge_songs", {{"token", token}}, kListTimeoutMs);
        if (status != 200) {
            spdlog::info("TaikoForge charts: not asked now (HTTP {})", status);
            return false;
        }
        rapidjson::Document doc;
        if (doc.Parse(body.c_str()).HasParseError() || !doc.IsObject() || !doc.HasMember("songs") ||
            !doc["songs"].IsArray()) {
            spdlog::warn("TaikoForge charts: the list is not what was expected");
            return false;
        }
        std::vector<Song> songs;
        for (const auto& s : doc["songs"].GetArray()) {
            if (!s.IsObject() || !s.HasMember("id") || !s["id"].IsString()) {
                continue;
            }
            const std::string id = s["id"].GetString();
            if (id.size() != 32 ||
                !std::ranges::all_of(id, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); })) {
                continue;
            }
            songs.push_back({id, s.HasMember("title") && s["title"].IsString() ? s["title"].GetString() : ""});
        }
        const bool paid = doc.HasMember("paid") && doc["paid"].IsBool() && doc["paid"].GetBool();
        const int days =
            doc.HasMember("retention_days") && doc["retention_days"].IsInt() ? doc["retention_days"].GetInt() : 30;

        const fs::path root = folder();
        std::error_code ec;
        bool changed = false;
        if (songs.empty() && !fs::exists(root, ec)) {
            return false; // no charts and never had any: no empty folder in the song list
        }
        if (!fs::exists(root, ec)) {
            fs::create_directories(root, ec);
            changed = true;
        }

        // the chart folders already here, by TaikoForge id (only folders with a .forge_id are ever ours)
        std::map<std::string, fs::path> have;
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (e.is_directory(ec) && fs::is_regular_file(e.path() / kIdFile, ec)) {
                have[read_file(e.path() / kIdFile)] = e.path();
            }
        }

        // how long the audio stays (TaikoForge's rule, as the web version shows it): free, N days after the chart;
        // starter and above, while the plan lasts and N days after it ends
        const std::string d = std::to_string(days);
        const std::string box =
            "#TITLE:TaikoForge\n#GENRE:TaikoForge\n#BOXCOLOR:#e8562a\n#FORECOLOR:#6a1f08\n" +
            (paid ? "#BOXEXPLANATION1:付費方案期間\n#BOXEXPLANATION2:音樂一直保存\n#BOXEXPLANATION3:結束 " + d +
                        " 天後刪除\n"
                  : "#BOXEXPLANATION1:免費方案：音樂\n#BOXEXPLANATION2:作譜 " + d +
                        " 天後刪除\n#BOXEXPLANATION3:入門以上可一直保存\n");
        changed |= write_if_changed(root / "box.def", box);

        // a folder per chart: the one it had (kept on a title change), else a free name
        std::set<std::string> used;
        for (const auto& [id, dir] : have) {
            used.insert(dir.filename().string());
        }
        std::vector<std::pair<Song, fs::path>> jobs;
        for (const Song& s : songs) {
            if (auto it = have.find(s.id); it != have.end()) {
                jobs.emplace_back(s, it->second);
                continue;
            }
            std::string name = folder_name(s.title);
            if (used.contains(name)) {
                name += " " + s.id.substr(0, 6);
            }
            used.insert(name);
            jobs.emplace_back(s, root / name);
        }

        progress.total = static_cast<int>(jobs.size());
        progress.done = 0;
        std::mutex mu;
        std::set<std::string> kept;
        size_t next = 0;
        auto worker = [&] {
            for (;;) {
                std::pair<Song, fs::path> job;
                {
                    std::scoped_lock lock(mu);
                    if (next >= jobs.size()) {
                        return;
                    }
                    job = jobs[next++];
                }
                const auto& [song, dir] = job;
                bool wrote = false;
                std::error_code e;
                const auto [ts, tja] = network.signed_get_now(
                    "/forge_song", {{"token", token}, {"id", song.id}, {"file", "tja"}}, kTjaTimeoutMs);
                const bool exists = fs::exists(dir / kIdFile, e);
                if (ts == 200 && !tja.empty()) {
                    if (!fs::exists(dir, e)) {
                        fs::create_directories(dir, e);
                    }
                    wrote |= write_if_changed(dir / kIdFile, song.id);
                    // song.tja beside song.ogg: a short name (the folder's title twice could pass Windows' 260
                    // characters in a deep song folder); any other .tja here would list the chart twice
                    for (const auto& f : fs::directory_iterator(dir, e)) {
                        if (f.path().extension() == ".tja" && f.path().filename() != kChart) {
                            fs::remove(f.path(), e);
                            wrote = true;
                        }
                    }
                    if (!write_if_changed(dir / kChart, with_local_wave(tja))) {
                        if (!fs::is_regular_file(dir / kChart, e)) {
                            spdlog::warn("TaikoForge chart '{}': could not write {}", song.title,
                                         (dir / kChart).string());
                        }
                    } else {
                        wrote = true;
                    }
                    if (!fs::exists(dir / kAudio, e) || fs::file_size(dir / kAudio, e) == 0) {
                        const auto [as, audio] = network.signed_get_now(
                            "/forge_song", {{"token", token}, {"id", song.id}, {"file", "audio"}}, kAudioTimeoutMs);
                        if (as == 200 && !audio.empty()) {
                            wrote |= write_if_changed(dir / kAudio, audio);
                        } else {
                            spdlog::warn("TaikoForge chart '{}': no audio (HTTP {})", song.title, as);
                        }
                    }
                    // a chart without its audio is not listed (nothing to play)
                    if (!fs::exists(dir / kAudio, e)) {
                        fs::remove_all(dir, e);
                        wrote |= exists;
                    } else {
                        std::scoped_lock lock(mu);
                        kept.insert(song.id);
                    }
                } else if (exists) {
                    // could not be asked now: keep what is here
                    std::scoped_lock lock(mu);
                    kept.insert(song.id);
                }
                if (wrote) {
                    std::scoped_lock lock(mu);
                    changed = true;
                }
                ++progress.done;
            }
        };
        std::vector<std::future<void>> pool;
        for (int i = 0; i < kWorkers; ++i) {
            pool.push_back(std::async(std::launch::async, worker));
        }
        for (auto& f : pool) {
            f.get();
        }

        // charts deleted on TaikoForge (the list was answered, so this is what it holds now)
        for (const auto& [id, dir] : have) {
            if (!kept.contains(id)) {
                fs::remove_all(dir, ec);
                changed = true;
            }
        }
        if (kept.empty() && songs.empty()) {
            fs::remove_all(root, ec); // nothing left: no empty TaikoForge folder
            changed = true;
        }
        spdlog::info("TaikoForge charts: {} listed, {} here{}", songs.size(), kept.size(), changed ? ", changed" : "");
        return changed;
    }

} // namespace zdon_forge_songs
