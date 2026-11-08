#include "filesystem.h"

#include "../library_index.h"
#include "../test_mode.h"
#include "../zdon_os.h"
#include "miniz.h" // IWYU pragma: keep
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#ifdef SUPPORT_FUMEN
#include "../optional/gen3.h"
#include "../optional/gen4.h"
#endif
#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <spdlog/spdlog.h>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../../platform/platform.h"
#include "../../platform/platform_windows.h"
#include "../asset_cache.h"
#include "../gfx.h"
#include "../jobs.h"

namespace {

    // Paths come from one completed scan. Consumers can count charts and collections
    // without walking the same directories again while a menu is being rendered.
    class SongFileSnapshot {
        using Path = std::filesystem::path;
        std::vector<Path> roots;
        std::vector<Path> files;
        std::vector<Path> definitions;
        std::vector<Path> osu_parent_folders;

        static bool within(const Path& path, const Path& root) {
            if (root == ".") {
                return !path.is_absolute() && !path.empty() && *path.begin() != "..";
            }
            auto p = path.begin();
            for (auto r = root.begin(); r != root.end(); ++r, ++p) {
                if (p == path.end() || *p != *r) {
                    return false;
                }
            }
            return !root.empty();
        }

    public:
        SongFileSnapshot(std::vector<Path> roots, std::vector<Path> files)
            : roots(std::move(roots)), files(std::move(files)) {
            auto normalize = [](Path& path) {
                path = path.lexically_normal();
                if (!path.empty() && path.filename().empty()) {
                    path = path.parent_path();
                }
            };
            for (auto& path : this->roots) {
                normalize(path);
            }
            for (auto& path : this->files) {
                normalize(path);
            }
            std::ranges::sort(this->files);
            const auto duplicates = std::ranges::unique(this->files);
            this->files.erase(duplicates.begin(), duplicates.end());
            for (const auto& file : this->files) {
                if (file.filename() == "box.def") {
                    definitions.push_back(file);
                } else if (file.extension() == ".osu") {
                    const auto directory = file.parent_path();
                    auto parent = directory.parent_path();
                    if (!directory.empty() && directory != parent) {
                        osu_parent_folders.push_back(parent.empty() ? Path(".") : std::move(parent));
                    }
                }
            }
            std::ranges::sort(osu_parent_folders);
            osu_parent_folders.erase(std::ranges::unique(osu_parent_folders).begin(), osu_parent_folders.end());
        }

        std::optional<ScannedFolderState> folder_state(Path path) const {
            path = path.lexically_normal();
            if (!path.empty() && path.filename().empty()) {
                path = path.parent_path();
            }
            if (std::ranges::none_of(roots, [&](const auto& root) { return within(path, root); })) {
                return std::nullopt;
            }
            auto definition = std::ranges::lower_bound(definitions, path);
            const bool has_definition = definition != definitions.end() && within(*definition, path);
            if (has_definition &&
                (definition->parent_path() == path || (path == "." && definition->parent_path().empty()))) {
                ++definition;
            }
            const bool has_children = (definition != definitions.end() && within(*definition, path)) ||
                                      std::ranges::binary_search(osu_parent_folders, path);
            return ScannedFolderState{has_definition, has_children};
        }

        std::optional<std::vector<Path>> under(Path path) const {
            path = path.lexically_normal();
            if (!path.empty() && path.filename().empty()) {
                path = path.parent_path();
            }
            if (std::ranges::none_of(roots, [&](const auto& root) { return within(path, root); })) {
                return std::nullopt;
            }
            std::vector<Path> result;
            if (path == ".") {
                for (const auto& file : files) {
                    if (within(file, path)) {
                        result.push_back(file);
                    }
                }
                return result;
            }
            // A folder's descendants form one contiguous range in component-wise path order.
            for (auto file = std::ranges::lower_bound(files, path); file != files.end() && within(*file, path);
                 ++file) {
                result.push_back(*file);
            }
            return result;
        }
    };

    // Native directory entries have no links. Keep I/O concurrency bounded rather
    // than spawning a thread for every directory in a large chart library.
    template<class List>
    std::optional<std::vector<PlatformFileEntry>> walk_native_directories(std::vector<PlatformFileEntry> root,
                                                                          List list, unsigned workers = 3) {
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<std::filesystem::path> pending;
        std::vector<PlatformFileEntry> files;
        unsigned active = 0;
        bool failed = false;
        auto append = [&](std::vector<PlatformFileEntry>& entries) {
            for (auto& entry : entries) {
                if (entry.directory) {
                    pending.push_back(std::move(entry.path));
                } else {
                    files.push_back(std::move(entry));
                }
            }
        };
        append(root);
        auto run = [&] {
            for (;;) {
                std::filesystem::path path;
                {
                    std::unique_lock lock(mutex);
                    wake.wait(lock, [&] { return failed || !pending.empty() || active == 0; });
                    if (failed || (pending.empty() && active == 0)) {
                        return;
                    }
                    path = std::move(pending.front());
                    pending.pop_front();
                    ++active;
                }
                std::vector<PlatformFileEntry> entries;
                bool ok = false;
                try {
                    ok = list(path, entries);
                } catch (...) {
                    ok = false;
                }
                {
                    std::scoped_lock lock(mutex);
                    failed |= !ok;
                    if (ok && !failed) {
                        try {
                            append(entries);
                        } catch (...) {
                            failed = true;
                        }
                    }
                    --active;
                }
                wake.notify_all();
            }
        };
        std::vector<std::thread> threads;
        try {
            for (unsigned i = 0; i < std::max(1u, workers); ++i) {
                threads.emplace_back(run);
            }
        } catch (...) {
            {
                std::scoped_lock lock(mutex);
                failed = true;
            }
            wake.notify_all();
        }
        for (auto& thread : threads) {
            thread.join();
        }
        if (failed) {
            return std::nullopt;
        }
        std::ranges::sort(files, {}, &PlatformFileEntry::path);
        return files;
    }

    std::mutex snapshot_mutex;
    std::shared_ptr<const SongFileSnapshot> song_file_snapshot;
} // namespace

std::optional<std::vector<fs::path>> get_scanned_song_entries(const fs::path& directory) {
    std::shared_ptr<const SongFileSnapshot> snapshot;
    {
        std::scoped_lock lock(snapshot_mutex);
        snapshot = song_file_snapshot;
    }
    return snapshot ? snapshot->under(directory) : std::nullopt;
}

std::optional<ScannedFolderState> get_scanned_folder_state(const fs::path& directory) {
    std::shared_ptr<const SongFileSnapshot> snapshot;
    {
        std::scoped_lock lock(snapshot_mutex);
        snapshot = song_file_snapshot;
    }
    return snapshot ? snapshot->folder_state(directory) : std::nullopt;
}

void set_working_directory_to_executable() {
    const std::filesystem::path exe_dir = platform_get_working_directory();
    if (exe_dir.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::current_path(exe_dir, ec);
    if (ec) {
        spdlog::error("Failed to set working directory to {}: {}", exe_dir.string(), ec.message());
        return;
    }
    spdlog::info("Working directory set to: {}", exe_dir.string());
}

static bool resolve_safe_extract_path(const fs::path& base_dir, const fs::path& canonical_base_dir,
                                      const std::string& entry_name, fs::path& out_file) {
    out_file = (base_dir / entry_name).lexically_normal();
    const fs::path rel = out_file.lexically_relative(base_dir);
    if (rel.empty() || *rel.begin() == "..") {
        return false;
    }

    std::error_code ec;
    fs::create_directories(out_file.parent_path(), ec);

    fs::path canonical_parent = fs::canonical(out_file.parent_path(), ec);
    if (ec) {
        return false;
    }
    const fs::path parent_rel = canonical_parent.lexically_relative(canonical_base_dir);
    return !(parent_rel.empty() || *parent_rel.begin() == "..");
}

// Extracts every file entry of `zip` under `base_dir`, rejecting entries that escape it. With a non-null
// `strip_prefix`, that leading folder is removed from entry names and entries left with an empty name are
// skipped. Returns false if any entry could not be read, was unsafe or failed to extract.
static bool extract_zip_entries(mz_zip_archive& zip, const fs::path& base_dir, const fs::path& canonical_base_dir,
                                const std::string* strip_prefix, const char* tag, const fs::path& archive_path) {
    bool all_ok = true;
    const int num_files = static_cast<int>(mz_zip_reader_get_num_files(&zip));
    for (int i = 0; i < num_files; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            all_ok = false;
            continue;
        }
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            continue;
        }

        std::string name = stat.m_filename;
        if (strip_prefix) {
            if (!strip_prefix->empty() && name.starts_with(*strip_prefix)) {
                name = name.substr(strip_prefix->size());
            }
            if (name.empty()) {
                continue;
            }
        }

        fs::path out_file;
        if (!resolve_safe_extract_path(base_dir, canonical_base_dir, name, out_file)) {
            spdlog::warn("{}: skipping unsafe entry {}", tag, stat.m_filename);
            all_ok = false;
            continue;
        }

        if (!mz_zip_reader_extract_to_file(&zip, i, out_file.string().c_str(), 0)) {
            spdlog::warn("{}: failed to extract {} from {}", tag, stat.m_filename, archive_path.string());
            all_ok = false;
        }
    }
    return all_ok;
}

void extract_osz(const fs::path& osz_path) {
    fs::path out_dir = osz_path.parent_path() / osz_path.stem();
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    fs::path canonical_out_dir = fs::canonical(out_dir, ec);
    if (ec) {
        spdlog::error("extract_osz: failed to resolve {}", out_dir.string());
        return;
    }

    mz_zip_archive zip = {};
    if (!mz_zip_reader_init_file(&zip, osz_path.string().c_str(), 0)) {
        spdlog::error("extract_osz: failed to open {}", osz_path.string());
        return;
    }

    const bool all_ok = extract_zip_entries(zip, out_dir, canonical_out_dir, nullptr, "extract_osz", osz_path);

    mz_zip_reader_end(&zip);
    if (all_ok) {
        fs::remove(osz_path, ec);
    } else {
        spdlog::warn("extract_osz: keeping {} because extraction was incomplete", osz_path.string());
    }
    spdlog::info("extract_osz: extracted {} to {}", osz_path.string(), out_dir.string());
}

void ensure_skin_extracted(const std::string& skin_name) {
    fs::path skin_dir = fs::path("Skins") / skin_name;
    fs::path marker = skin_dir / ".extracted";
    std::error_code ec;
    if (fs::exists(marker, ec)) {
        return;
    }

    fs::path zip_path = fs::path("Skins") / (skin_name + ".zip");
    if (!fs::exists(zip_path, ec)) {
        return;
    }

    mz_zip_archive zip = {};
    if (!mz_zip_reader_init_file(&zip, zip_path.string().c_str(), 0)) {
        spdlog::error("ensure_skin_extracted: failed to open {}", zip_path.string());
        return;
    }

    const auto num_files = mz_zip_reader_get_num_files(&zip);

    std::string common_prefix;
    bool prefix_initialized = false;
    for (auto i = 0u; i < num_files; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            continue;
        }
        std::string name = stat.m_filename;
        auto slash = name.find('/');
        if (slash == std::string::npos) {
            common_prefix.clear();
            break;
        }
        std::string top = name.substr(0, slash + 1);
        if (!prefix_initialized) {
            common_prefix = top;
            prefix_initialized = true;
        } else if (top != common_prefix) {
            common_prefix.clear();
            break;
        }
    }

    fs::create_directories(skin_dir, ec);
    fs::path canonical_skin_dir = fs::canonical(skin_dir, ec);
    if (ec) {
        spdlog::error("ensure_skin_extracted: failed to resolve {}", skin_dir.string());
        mz_zip_reader_end(&zip);
        return;
    }
    extract_zip_entries(zip, skin_dir, canonical_skin_dir, &common_prefix, "ensure_skin_extracted", zip_path);

    mz_zip_reader_end(&zip);
    spdlog::info("ensure_skin_extracted: extracted {} to {}", zip_path.string(), skin_dir.string());

    std::ofstream marker_file(marker, std::ios::trunc);
    if (!marker_file) {
        spdlog::warn("ensure_skin_extracted: failed to write completion marker for {}", skin_name);
    }
}

std::vector<std::string> list_available_skins() {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::path("Skins"), ec)) {
        if (e.is_directory(ec) && fs::exists(e.path() / "Graphics", ec)) {
            names.push_back(e.path().filename().string());
        } else if (e.path().extension() == ".zip") {
            names.push_back(e.path().stem().string());
        }
    }
    std::ranges::sort(names);
    names.erase(std::ranges::unique(names).begin(), names.end());
    return names;
}

// True if `dir` or any of its ancestors is a folder named "fumen".
static bool path_is_under_fumen(fs::path dir) {
    for (; !dir.empty() && dir != dir.parent_path(); dir = dir.parent_path()) {
        if (dir.filename() == "fumen") {
            return true;
        }
    }
    return false;
}

// Files in a "fumen" folder are only charts when they are .bin (gen3/gen4 data).
static void add_chart_file(std::vector<fs::path>& songs, std::vector<fs::path>* osz_out, const fs::path& p,
                           bool under_fumen, std::vector<SongFileStamp>* stamps = nullptr,
                           const SongFileStamp& stamp = {}) {
    const fs::path ext = p.extension();
    if (ext == ".tja" || ext == ".osu" || (ext == ".bin" && under_fumen)) {
        songs.push_back(p);
        if (stamps) {
            stamps->push_back(stamp);
        }
    } else if (ext == ".osz") {
        if (osz_out) {
            osz_out->push_back(p);
        }
    }
}

static void collect_navigation_file(const fs::path& path, std::vector<fs::path>& files) {
    auto name = path.filename().string();
    if (name == "song_list.txt" || name == "box.def") {
        files.push_back(path);
        return;
    }
    if (name.size() != 7) {
        return;
    }
    std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
    if (name == "box.def") {
        const auto definition = path.parent_path() / "box.def";
        std::error_code ec;
        if (fs::exists(definition, ec)) {
            files.push_back(definition);
        }
    }
}

#ifdef _WIN32
// Windows walk for collect_charts_from. recursive_directory_iterator stats every entry and
// fs::canonical stats every component of every folder, which took seconds on a library of a few
// thousand folders; a directory listing already carries each entry's attributes. Same paths in the
// same order as the portable walk below.
static SongFileStamp stamp_of(const Win32DirEntry& e) {
    return {true, e.is_dir, e.is_reparse, static_cast<int64_t>(e.size), static_cast<int64_t>(e.ft_write)};
}

// the index's copy of a listing, without the names: the walk still reads them, and moves them in
// once it is done with the folder (move_names)
static std::vector<LibraryIndex::Entry> index_entries(const std::vector<Win32DirEntry>& entries) {
    std::vector<LibraryIndex::Entry> out;
    out.reserve(entries.size());
    for (const Win32DirEntry& e : entries) {
        out.push_back({{}, e.is_dir, e.is_reparse, -1});
    }
    return out;
}

static void move_names(LibraryIndex* index, int32_t dir, std::vector<Win32DirEntry>& entries) {
    if (index && dir >= 0) {
        std::vector<LibraryIndex::Entry>& out = index->entries_of(dir);
        for (size_t k = 0; k < entries.size(); k++) {
            out[k].name = std::move(entries[k].name);
        }
    }
}

struct ChartWalk {
    std::vector<fs::path>& songs;
    std::vector<fs::path>* osz_out;
    std::vector<SongFileStamp>* stamps;
    std::vector<fs::path>& song_lists; // merge 0ed42ec: upstream's song_list.txt / box.def collection
    // Resolved path of every folder entered: a link back to one of them is not followed again.
    // A plain folder resolves to its parent's resolved path plus its name, so only links are opened.
    std::unordered_set<std::wstring> visited_dirs;
    // When set, every listing is recorded (`dir_id`: dir's record); a link to a folder entered
    // before points at that folder's record (by resolved path)
    LibraryIndex* index;
    std::unordered_map<std::wstring, int32_t> index_by_resolved;

    // `entries` is the listing of `dir`; `resolved` is dir's resolved path
    void walk(const fs::path& dir, const std::wstring& resolved, std::vector<Win32DirEntry>& entries, bool under_fumen,
              int32_t dir_id) {
        for (size_t k = 0; k < entries.size(); k++) {
            const Win32DirEntry& e = entries[k];
            fs::path p = dir / e.name;
            if (!e.is_dir) {
                collect_navigation_file(p, song_lists);
                add_chart_file(songs, osz_out, p, under_fumen, stamps, stamp_of(e));
                continue;
            }
            std::wstring child_resolved = e.is_reparse ? win32_final_path(p) : resolved + L'\\' + e.name;
            if (child_resolved.empty()) {
                // Cannot prove this is not a loop -> do not recurse into it.
                spdlog::warn("collect_charts_from: cannot canonicalize {}, skipping recursion", p.string());
                continue;
            }
            if (!visited_dirs.insert(child_resolved).second) {
                if (index) {
                    if (auto it = index_by_resolved.find(child_resolved); it != index_by_resolved.end()) {
                        index->set_child(dir_id, k, it->second);
                    }
                }
                continue;
            }

            std::vector<Win32DirEntry> children;
            const bool listed = win32_list_dir(p, children);
            int32_t child_id = -1;
            if (index && listed) {
                child_id = index->add_dir(p, index_entries(children));
                index->set_child(dir_id, k, child_id);
                index_by_resolved.emplace(child_resolved, child_id);
            }
#ifdef SUPPORT_FUMEN
            // Both data roots have a fumen folder: only then ask them
            bool has_fumen = false;
            for (const Win32DirEntry& c : children) {
                if (c.is_dir && _wcsicmp(c.name.c_str(), L"fumen") == 0) {
                    has_fumen = true;
                    break;
                }
            }
            if (has_fumen && (gen4::find_data_root(p) == p || gen3::find_data_root(p) == p)) {
                move_names(index, child_id, children); // indexed without its subfolders
                continue;
            }
#endif
            add_chart_file(songs, osz_out, p, under_fumen, stamps, stamp_of(e));
            if (listed) {
                walk(p, child_resolved, children, under_fumen || e.name == L"fumen", child_id);
            }
        }
        move_names(index, dir_id, entries);
    }
};
#elif defined(ZDON_POSIX_LIST_DIR)
// The same walk on Linux, Android and macOS/iOS: readdir gives each entry's type, so only links,
// entries of unknown type and the chart files (their size and write time, for the song cache) are
// stat'ed; the portable walk below stat'ed every entry and realpath'ed every folder, and the loading
// screen stat'ed every chart twice more. Same paths in the same order as the portable walk (readdir
// order, a folder before its contents). No library index (its names are wide strings, Windows only).
static bool wants_stamp(std::string_view name) {
    auto ends = [&](std::string_view ext) {
        return name.size() > ext.size() && name.substr(name.size() - ext.size()) == ext;
    };
    return ends(".tja") || ends(".osu") || ends(".bin");
}

static SongFileStamp stamp_of(const zdon_os::DirEntry& e) {
    if (!e.is_dir && !e.stat_ok) {
        return {}; // not listed: the loading screen stats it
    }
    return {true, e.is_dir, e.is_link, static_cast<int64_t>(e.size), static_cast<int64_t>(e.ft_write)};
}

struct PosixChartWalk {
    std::vector<fs::path>& songs;
    std::vector<fs::path>* osz_out;
    std::vector<SongFileStamp>* stamps;
    // Resolved path of every folder entered: a link back to one of them is not followed again.
    // A plain folder resolves to its parent's resolved path plus its name, so only links are resolved.
    std::unordered_set<std::string> visited_dirs;

    void walk(const fs::path& dir, const std::string& resolved, std::vector<zdon_os::DirEntry>& entries,
              bool under_fumen) {
        for (const zdon_os::DirEntry& e : entries) {
            fs::path p = dir / e.name;
            if (!e.is_dir) {
                add_chart_file(songs, osz_out, p, under_fumen, stamps, stamp_of(e));
                continue;
            }
            std::string child_resolved;
            if (e.is_link) {
                std::error_code ec;
                child_resolved = fs::canonical(p, ec).string();
                if (ec) {
                    child_resolved.clear();
                }
            } else {
                child_resolved = resolved + (resolved.ends_with('/') ? "" : "/") + e.name;
            }
            if (child_resolved.empty()) {
                // Cannot prove this is not a loop -> do not recurse into it.
                spdlog::warn("collect_charts_from: cannot canonicalize {}, skipping recursion", p.string());
                continue;
            }
            if (!visited_dirs.insert(child_resolved).second) {
                continue;
            }
#ifdef SUPPORT_FUMEN
            if (gen4::find_data_root(p) == p || gen3::find_data_root(p) == p) {
                continue;
            }
#endif
            add_chart_file(songs, osz_out, p, under_fumen, stamps, stamp_of(e));
            std::vector<zdon_os::DirEntry> children;
            if (zdon_os::list_dir(p, children, stamps ? wants_stamp : nullptr)) {
                walk(p, child_resolved, children, under_fumen || e.name == "fumen");
            }
        }
    }
};
#endif

static void collect_charts_from(const fs::path& path, std::vector<fs::path>& songs, std::vector<fs::path>* osz_out,
                                std::vector<SongFileStamp>* stamps, LibraryIndex* index,
                                std::vector<fs::path>& song_lists) {
    // upstream: a platform with its own directory listing (Switch) walks with it; Windows / Linux return false
    std::vector<PlatformFileEntry> native_entries;
    const bool native_listed = platform_list_directory(path, native_entries);
    auto native_files =
        native_listed ? walk_native_directories(std::move(native_entries), platform_list_directory) : std::nullopt;
    if (native_files) {
        for (const auto& entry : *native_files) {
            collect_navigation_file(entry.path, song_lists);
            // Z-DON: the shared chart filter; the native listing has no stamps (the loader stats those files)
            add_chart_file(songs, osz_out, entry.path,
                           entry.path.extension() == ".bin" && path_is_under_fumen(entry.path.parent_path()), stamps);
        }
        return;
    }
    if (native_listed) {
        spdlog::warn("Native chart walk failed for {}; retrying with the portable walker", path.string());
    }
#ifdef _WIN32
    std::vector<Win32DirEntry> entries;
    if (!win32_list_dir(path, entries)) {
        throw fs::filesystem_error("cannot open directory", path,
                                   std::make_error_code(std::errc::no_such_file_or_directory));
    }
    const bool under_fumen = path_is_under_fumen(path);
    ChartWalk w{songs, osz_out, stamps, song_lists, {}, index, {}};
    std::wstring root = win32_final_path(path);
    if (root.empty()) {
        root = fs::absolute(path).wstring();
    }
    w.visited_dirs.insert(root);
    int32_t root_id = -1;
    if (index) {
        root_id = index->add_dir(path, index_entries(entries));
        w.index_by_resolved.emplace(root, root_id);
    }
    w.walk(path, root, entries, under_fumen, root_id);
#elif defined(ZDON_POSIX_LIST_DIR)
    (void)index;
    std::vector<zdon_os::DirEntry> entries;
    if (!zdon_os::list_dir(path, entries, stamps ? wants_stamp : nullptr)) {
        throw fs::filesystem_error("cannot open directory", path,
                                   std::make_error_code(std::errc::no_such_file_or_directory));
    }
    PosixChartWalk w{songs, osz_out, stamps, {}};
    std::error_code canon_ec;
    std::string root = fs::canonical(path, canon_ec).string();
    if (canon_ec) {
        root = fs::absolute(path).lexically_normal().string();
    } else {
        w.visited_dirs.insert(root);
    }
    w.walk(path, root, entries, path_is_under_fumen(path));
#else
    (void)index;
    // A symlinked directory that points back at one of its own ancestors would
    // otherwise make the recursive iterator loop forever. Track the canonical
    // path of every directory entered so a symlink resolving to one of them
    // can be caught before we recurse into it again.
    std::unordered_set<std::string> visited_dirs;
    std::error_code canon_ec;
    {
        fs::path root_canonical = fs::canonical(path, canon_ec);
        if (!canon_ec) {
            visited_dirs.insert(root_canonical.string());
        }
    }
    std::error_code it_ec;
    auto it = fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied |
                                                         fs::directory_options::follow_directory_symlink);
    for (; it != fs::end(it); it.increment(it_ec)) {
        if (it_ec) {
            spdlog::warn("collect_charts_from: stopping scan of {} after iteration error: {}", path.string(),
                         it_ec.message());
            break;
        }
        const auto& entry = *it;

        std::error_code dir_ec;
        bool is_dir = entry.is_directory(dir_ec);
        if (is_dir) {
            fs::path entry_canonical = fs::canonical(entry.path(), canon_ec);
            if (canon_ec) {
                // Cannot prove this is not a loop -> do not recurse into it.
                spdlog::warn("collect_charts_from: cannot canonicalize {}, skipping recursion", entry.path().string());
                it.disable_recursion_pending();
                continue;
            }
            if (!visited_dirs.insert(entry_canonical.string()).second) {
                it.disable_recursion_pending();
                continue;
            }
        }

#ifdef SUPPORT_FUMEN
        if (is_dir && (gen4::find_data_root(entry.path()) == entry.path() ||
                       gen3::find_data_root(entry.path()) == entry.path())) {
            it.disable_recursion_pending();
            continue;
        }
#endif

        if (!is_dir) {
            collect_navigation_file(entry.path(), song_lists);
        }
        add_chart_file(songs, osz_out, entry.path(),
                       entry.path().extension() == ".bin" && path_is_under_fumen(entry.path().parent_path()), stamps);
    }
#endif
}

std::vector<fs::path> get_song_files(const std::vector<fs::path>& root_path, std::vector<SongFileStamp>* stamps_out,
                                     std::shared_ptr<const LibraryIndex>* index_out) {
    std::shared_ptr<LibraryIndex> index;
#ifdef _WIN32
    if (index_out) {
        index = std::make_shared<LibraryIndex>();
    }
#endif
    platform_begin_file_scan();
    std::vector<fs::path> songs;
    std::vector<SongFileStamp> stamps;
    std::vector<SongFileStamp>* stamps_ptr = stamps_out ? &stamps : nullptr;
    std::vector<fs::path> song_lists, covered_roots;
    for (const fs::path& path : root_path) {
#ifdef SUPPORT_FUMEN
        if (!gen4::find_data_root(path).empty() || !gen3::find_data_root(path).empty()) {
            continue;
        }
#endif

        std::vector<fs::path> osz_files;
        try {
            collect_charts_from(path, songs, &osz_files, stamps_ptr, index.get(), song_lists);
            covered_roots.push_back(path);
        } catch (const std::filesystem::filesystem_error& e) {
            spdlog::error("Error scanning song directory: {}", e.what());
            continue;
        }

        if (!osz_files.empty()) {
            index.reset(); // the extracted folders are not in their parent's listing
            for (const auto& osz : osz_files) {
                extract_osz(osz);
            }
            for (const auto& osz : osz_files) {
                fs::path out_dir = osz.parent_path() / osz.stem();
                std::error_code ec;
                if (!fs::exists(out_dir, ec)) {
                    continue;
                }
                try {
                    collect_charts_from(out_dir, songs, nullptr, stamps_ptr, nullptr, song_lists);
                } catch (const std::filesystem::filesystem_error& e) {
                    spdlog::error("Error scanning extracted archive {}: {}", out_dir.string(), e.what());
                }
            }
        }
    }
    std::unordered_set<std::string> seen;
    std::vector<fs::path> deduped;
    deduped.reserve(songs.size());
    if (stamps_out) {
        stamps_out->clear();
        stamps_out->reserve(songs.size());
    }
    for (size_t i = 0; i < songs.size(); i++) {
        if (seen.insert(songs[i].string()).second) {
            deduped.push_back(std::move(songs[i]));
            if (stamps_out) {
                stamps_out->push_back(stamps[i]);
            }
        }
    }
    if (index_out) {
        *index_out = std::move(index);
    }
    // upstream's scan snapshot (folder_scan / navigation_scan): the charts plus every song_list.txt / box.def.
    // Z-DON: sorted in a background job (~30 ms for 3000 charts off the loading path); until it is published
    // its readers get nullopt and read the disk, as they do outside the scanned roots
    auto snapshot_entries = deduped;
    snapshot_entries.insert(snapshot_entries.end(), song_lists.begin(), song_lists.end());
    jobs::submit(jobs::Priority::Background,
                 [roots = std::move(covered_roots), entries = std::move(snapshot_entries)]() mutable {
                     auto snapshot = std::make_shared<SongFileSnapshot>(std::move(roots), std::move(entries));
                     std::scoped_lock lock(snapshot_mutex);
                     song_file_snapshot = std::move(snapshot);
                 });
    return deduped;
}

// Parsed skin JSON (texture.json etc.) by path, kept across screens.
static AssetCache<rapidjson::Document>& skin_json_cache() {
    static AssetCache<rapidjson::Document> cache(asset_cache_budget(16u << 20));
    return cache;
}

std::shared_ptr<const rapidjson::Document> read_json_shared(const fs::path& file_path) {
    if (skin_json_cache().enabled()) {
        const std::string cache_key = file_path.lexically_normal().generic_string();
        if (cache_key.starts_with("Skins/") || cache_key.contains("/Skins/")) {
            if (auto cached = skin_json_cache().get(cache_key)) {
                return cached;
            }
        }
    }
    return std::make_shared<const rapidjson::Document>(read_json_file(file_path)); // parses (and caches) it
}

rapidjson::Document read_json_file(const fs::path& file_path) {
    if (file_path.extension() != ".json") {
        throw std::runtime_error("File is not a json file: " + file_path.string());
    }

    const std::string cache_key = file_path.lexically_normal().generic_string();
    const bool cacheable =
        skin_json_cache().enabled() && (cache_key.starts_with("Skins/") || cache_key.contains("/Skins/"));
    if (cacheable) {
        if (auto cached = skin_json_cache().get(cache_key)) {
            rapidjson::Document copy;
            copy.CopyFrom(*cached, copy.GetAllocator());
            return copy;
        }
    }
    const std::vector<unsigned char> bytes = load_file_bytes(file_path.string().c_str());
    if (bytes.empty()) {
        throw std::runtime_error((fs::exists(file_path) ? "Failed to open file: " : "File not found: ") +
                                 file_path.string());
    }
    rapidjson::Document doc;
    doc.Parse(reinterpret_cast<const char*>(bytes.data()), bytes.size());

    if (doc.HasParseError()) {
        throw std::runtime_error("Failed to parse " + file_path.string() + ": " + std::to_string(doc.GetParseError()));
    }
    if (cacheable) {
        auto cached = std::make_shared<rapidjson::Document>();
        cached->CopyFrom(doc, cached->GetAllocator());
        const size_t bytes = sizeof(rapidjson::Document) + cached->GetAllocator().Capacity();
        skin_json_cache().put(cache_key, std::move(cached), bytes);
    }

    return doc;
}

bool write_json_file(const fs::path& file_path, const rapidjson::Document& doc) {
    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    writer.SetIndent(' ', 4);
    writer.SetFormatOptions(rapidjson::kFormatSingleLineArray);
    doc.Accept(writer);

    std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
    out << buffer.GetString() << '\n';
    if (!out) {
        spdlog::error("Failed to write {}", file_path.string());
        return false;
    }
    skin_json_cache().put(file_path.lexically_normal().generic_string(), nullptr, 0);
    return true;
}

namespace {
    fs::path g_skin_graphics_path;
    std::vector<fs::path> g_skin_chain; // Graphics paths: the skin, its parent, the parent's parent ...
    std::mutex g_skin_path_mutex;

    fs::path skin_root(const fs::path& graphics_path) {
        return graphics_path.parent_path();
    }
} // namespace

fs::path resolve_parent_graphics_path(const fs::path& graphics_path) {
    rapidjson::Document skin_config_file;
    try {
        skin_config_file = read_json_file(graphics_path / "skin_config.json");
    } catch (const std::exception& e) {
        spdlog::warn("resolve_parent_graphics_path: {}", e.what());
        return graphics_path;
    }
    if (skin_config_file.IsObject() && skin_config_file.HasMember("screen") && skin_config_file["screen"].IsObject() &&
        skin_config_file["screen"].HasMember("parent") && skin_config_file["screen"]["parent"].IsString()) {
        std::string parent = skin_config_file["screen"]["parent"].GetString();
        fs::path skins_root("Skins");
        fs::path candidate = (skins_root / parent).lexically_normal();
        fs::path rel = candidate.lexically_relative(skins_root);
        if (rel.empty() || *rel.begin() == "..") {
            spdlog::warn("resolve_parent_graphics_path: rejecting unsafe parent skin path '{}'", parent);
            return graphics_path;
        }
        ensure_skin_extracted(parent);
        return candidate / "Graphics";
    }
    return graphics_path;
}

std::vector<fs::path> resolve_skin_chain(const fs::path& graphics_path) {
    std::vector<fs::path> chain{graphics_path};
    for (int depth = 0; depth < 8; depth++) {
        fs::path parent = resolve_parent_graphics_path(chain.back());
        if (parent == chain.back()) {
            break;
        }
        if (std::ranges::contains(chain, parent)) {
            spdlog::warn("resolve_skin_chain: parent cycle at '{}'", parent.string());
            break;
        }
        // A parent that is not installed ends the chain instead of failing the whole skin: a child that
        // ships everything it draws (e.g. one built on a skin whose own parent the user never installed)
        // still loads.
        std::error_code ec;
        if (!fs::is_regular_file(parent / "skin_config.json", ec)) {
            spdlog::warn("resolve_skin_chain: parent skin '{}' is not installed; the chain stops at '{}'",
                         parent.parent_path().string(), chain.back().string());
            break;
        }
        chain.push_back(parent);
    }
    return chain;
}

void set_skin_graphics_path(const fs::path& graphics_path) {
    std::vector<fs::path> chain = resolve_skin_chain(graphics_path);
    std::scoped_lock lock(g_skin_path_mutex);
    g_skin_graphics_path = graphics_path;
    g_skin_chain = std::move(chain);
}

std::vector<fs::path> skin_ancestor_roots() {
    std::scoped_lock lock(g_skin_path_mutex);
    std::vector<fs::path> roots;
    for (size_t i = 1; i < g_skin_chain.size(); i++) {
        roots.push_back(skin_root(g_skin_chain[i]));
    }
    return roots;
}

fs::path resolve_skin_path(const fs::path& relative_path) {
    fs::path child_root;
    std::vector<fs::path> chain;
    {
        std::scoped_lock lock(g_skin_path_mutex);
        child_root = skin_root(g_skin_graphics_path);
        chain = g_skin_chain;
    }
    fs::path child = child_root / relative_path;
    if (fs::exists(child)) {
        return child;
    }
    for (size_t i = 1; i < chain.size(); i++) {
        fs::path ancestor = skin_root(chain[i]) / relative_path;
        if (fs::exists(ancestor)) {
            return ancestor;
        }
    }
    return child;
}
