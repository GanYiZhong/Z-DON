#include "zdon_song_share.h"

#include <algorithm>
#include <fstream>
#include <spdlog/spdlog.h>
#include <vector>

#include "file/filesystem.h"
#include "file/scores.h"
#include "global_data.h"
#include "library_index.h"
#include "miniz.h" // IWYU pragma: keep
#include "parsers/song_parser.h"

namespace {
    bool is_chart(const fs::path& p) {
        std::string ext = p.extension().string();
        std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext == ".tja";
    }

    std::string utf8(const fs::path& p) {
        const std::u8string s = p.generic_u8string();
        return {s.begin(), s.end()};
    }

    bool read_file(const fs::path& p, std::string& out) {
        std::ifstream f(p, std::ios::binary);
        if (!f) {
            return false;
        }
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return static_cast<bool>(f) || f.eof();
    }

    // a folder name that is safe on every system
    std::string safe_name(const std::string& name) {
        std::string out;
        for (const char c : name) {
            const auto u = static_cast<unsigned char>(c);
            if (u < 0x20 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) {
                out += '_';
            } else {
                out += c;
            }
        }
        while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
            out.pop_back();
        }
        while (!out.empty() && (out.front() == '.' || out.front() == ' ')) {
            out.erase(out.begin());
        }
        return out.empty() ? "song" : out.substr(0, 100);
    }

    fs::path online_folder() {
        const auto& roots = global_data.config->paths.tja_path;
        return (roots.empty() ? fs::path("Songs") : roots.front()) / "Z-DON Online";
    }
} // namespace

ZdonSongPack zdon_pack_song(const fs::path& chart) {
    ZdonSongPack out;
    std::error_code ec;
    if (!is_chart(chart) || !fs::is_regular_file(chart, ec)) {
        out.error = "not_tja";
        return out;
    }
    const fs::path folder = chart.parent_path();
    int charts_here = 0;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file() && is_chart(it->path())) {
            ++charts_here;
        }
    }
    std::vector<fs::path> files;
    if (charts_here <= 1) { // the song's own folder: all of it
        for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file()) {
                files.push_back(it->path());
            }
        }
        out.name = utf8(folder.filename());
    } else { // a folder of several charts: this one and what it plays
        files.push_back(chart);
        SongParser parser(chart);
        for (fs::path p : {parser.metadata.wave, parser.metadata.bgmovie}) {
            if (p.empty()) {
                continue;
            }
            if (p.is_relative()) {
                p = folder / p;
            }
            if (fs::is_regular_file(p, ec) && std::ranges::find(files, p) == files.end()) {
                files.push_back(p);
            }
        }
        out.name = utf8(chart.stem());
    }
    uintmax_t total = 0;
    for (const fs::path& f : files) {
        total += fs::file_size(f, ec);
    }
    if (total > kZdonSongShareMax) {
        out.error = "too_big";
        return out;
    }

    mz_zip_archive zip = {};
    if (!mz_zip_writer_init_heap(&zip, 0, static_cast<size_t>(total) + 64 * 1024)) {
        out.error = "read";
        return out;
    }
    bool ok = true;
    std::string data;
    for (const fs::path& f : files) {
        const std::string entry = utf8(f.lexically_relative(folder));
        if (entry.empty() || entry.starts_with("..") || !read_file(f, data)) {
            ok = false;
            break;
        }
        // audio and video don't shrink: stored as they are; the chart itself is small either way
        if (!mz_zip_writer_add_mem(&zip, entry.c_str(), data.data(), data.size(), MZ_NO_COMPRESSION)) {
            ok = false;
            break;
        }
    }
    void* buf = nullptr;
    size_t size = 0;
    if (ok && mz_zip_writer_finalize_heap_archive(&zip, &buf, &size)) {
        out.zip.assign(static_cast<const char*>(buf), size);
    } else {
        out.error = "read";
    }
    mz_zip_writer_end(&zip); // frees buf
    spdlog::info("Online battle: packed {} ({} files, {} bytes){}", utf8(chart), files.size(), out.zip.size(),
                 out.error.empty() ? "" : " failed: " + out.error);
    return out;
}

ZdonSongInstall zdon_install_song(const std::string& zip, const std::string& name, const std::string& want_hash) {
    ZdonSongInstall out;
    std::error_code ec;
    const fs::path base = online_folder();
    fs::create_directories(base, ec);
    if (!fs::exists(base / "box.def", ec)) {
        std::ofstream def(base / "box.def", std::ios::binary);
        def << "#TITLE:Z-DON Online\n#GENRE:Z-DON Online\n#BACKCOLOR:#3070C0\n#FORECOLOR:#1A3A70\n";
    }
    const std::string folder_name = safe_name(name);
    const fs::path dir = base / fs::path(std::u8string(folder_name.begin(), folder_name.end()));
    fs::remove_all(dir, ec); // an earlier copy of the same name: replaced
    const fs::path tmp = fs::path("cache") / "zdon_online_song.zip";
    fs::create_directories(tmp.parent_path(), ec);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(zip.data(), static_cast<std::streamsize>(zip.size()));
    }
    const bool extracted = zdon_extract_zip(tmp, dir);
    fs::remove(tmp, ec);
    if (!extracted) {
        fs::remove_all(dir, ec);
        out.error = "extract";
        return out;
    }
    // the chart whose course has the host's hash
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file() || !is_chart(it->path())) {
            continue;
        }
        SongParser parser(it->path());
        std::array<std::string, 5> hashes{};
        bool match = false;
        for (const auto& [course, data] : parser.metadata.course_data) {
            if (course >= 0 && course < 5) {
                hashes[course] = parser.get_diff_hash(course);
                match = match || (!want_hash.empty() && hashes[course] == want_hash);
            }
        }
        if (match) {
            scores_manager.add_path_binding(it->path(), hashes);
            if (auto index = library_index()) { // the folders changed since the startup walk listed them
                index->note_changed(base.parent_path());
                index->note_changed(base);
                index->note_changed(dir);
            }
            out.chart = it->path();
            spdlog::info("Online battle: installed {}", utf8(it->path()));
            return out;
        }
        out.error = "hash";
    }
    if (out.error.empty()) {
        out.error = "no_chart";
    }
    fs::remove_all(dir, ec);
    spdlog::warn("Online battle: the received song was not installed ({})", out.error);
    return out;
}
