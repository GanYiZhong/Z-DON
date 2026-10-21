#include "skin_updater.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <ranges>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "android_ca.h"
#include "sha256.h"

#ifndef __EMSCRIPTEN__

#include <cpr/cpr.h>

#ifdef _WIN32
#include "../platform/platform_windows.h"
#endif

#if defined(__ANDROID__)
#include <curl/curl.h>
#endif

namespace fs = std::filesystem;

namespace {

    std::string strip_dot_git(std::string url) {
        if (url.size() >= 4 && url.ends_with(".git")) {
            url.resize(url.size() - 4);
        }
        return url;
    }

    void trim(std::string& s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
            s.pop_back();
        }
    }

    void init_session(cpr::Session& session) {
#if defined(__ANDROID__)
        session.SetOption(android_ca());
        curl_easy_setopt(session.GetCurlHolder()->handle, CURLOPT_SSL_EC_CURVES, "X25519:P-256:P-384");
#else
        (void)session;
#endif
    }

    // Hashes the file in pieces rather than reading all of it into memory. Empty on a read error.
    std::string sha256_file(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return {};
        }
        crypto::Sha256 ctx;
        std::vector<char> buf(1 << 20);
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const std::streamsize got = in.gcount();
            if (got > 0) {
                ctx.update(reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(got));
            }
        }
        if (in.bad()) {
            return {};
        }
        return crypto::to_hex(ctx.finalize());
    }

    // The hash of each skin file as of its last check, so a pass reads only the files whose size or
    // modification time changed since. Kept in cache/skin_hashes/<skin>.txt, one
    // "<hash> <size> <mtime> <path>" line per file (the path last: it may contain spaces).
    struct HashCacheEntry {
        std::string hash;
        uintmax_t size = 0;
        int64_t mtime = 0;
        bool operator==(const HashCacheEntry&) const = default;
    };
    using HashCache = std::unordered_map<std::string, HashCacheEntry>;

    fs::path hash_cache_path(const fs::path& skin_dir) {
        return fs::path("cache") / "skin_hashes" / (skin_dir.filename().string() + ".txt");
    }

    HashCache load_hash_cache(const fs::path& skin_dir) {
        HashCache cache;
        std::ifstream in(hash_cache_path(skin_dir));
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream fields(line);
            HashCacheEntry e;
            if (!(fields >> e.hash >> e.size >> e.mtime)) {
                continue;
            }
            std::string rel;
            std::getline(fields >> std::ws, rel);
            if (!rel.empty()) {
                cache[rel] = std::move(e);
            }
        }
        return cache;
    }

    void save_hash_cache(const fs::path& skin_dir, const HashCache& cache) {
        const fs::path file = hash_cache_path(skin_dir);
        const fs::path tmp = fs::path(file).concat(".tmp");
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out) {
                return;
            }
            for (const auto& [rel, e] : cache) {
                out << e.hash << ' ' << e.size << ' ' << e.mtime << ' ' << rel << '\n';
            }
            if (!out) {
                return;
            }
        }
        fs::rename(tmp, file, ec);
        if (ec) {
            fs::remove(tmp, ec);
        }
    }

    // Z-DON: the manifest hash each file had when it was last installed or found up to date (the
    // "base"), and the hash of what was installed then. A file whose manifest hash still equals its
    // base hasn't changed upstream; one whose local hash differs from what was installed was edited
    // here. Kept in cache/skin_hashes/<skin>.base, one "<manifest hash> <installed hash> <path>" line
    // per file; the .txt hash cache keeps its upstream format.
    struct BaseEntry {
        std::string manifest_hash;
        std::string installed_hash;
        bool operator==(const BaseEntry&) const = default;
    };
    using BaseMap = std::unordered_map<std::string, BaseEntry>;

    fs::path base_path(const fs::path& skin_dir) {
        return fs::path("cache") / "skin_hashes" / (skin_dir.filename().string() + ".base");
    }

    BaseMap load_bases(const fs::path& skin_dir) {
        BaseMap bases;
        std::ifstream in(base_path(skin_dir));
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream fields(line);
            BaseEntry e;
            if (!(fields >> e.manifest_hash >> e.installed_hash)) {
                continue;
            }
            std::string rel;
            std::getline(fields >> std::ws, rel);
            if (!rel.empty()) {
                bases[rel] = std::move(e);
            }
        }
        return bases;
    }

    void save_bases(const fs::path& skin_dir, const BaseMap& bases) {
        const fs::path file = base_path(skin_dir);
        const fs::path tmp = fs::path(file).concat(".tmp");
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out) {
                return;
            }
            for (const auto& [rel, e] : bases) {
                out << e.manifest_hash << ' ' << e.installed_hash << ' ' << rel << '\n';
            }
            if (!out) {
                return;
            }
        }
        fs::rename(tmp, file, ec);
        if (ec) {
            fs::remove(tmp, ec);
        }
    }

    // "YYYYmmdd-HHMMSS", local time
    std::string timestamp_now() {
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
        return buf;
    }

    // The manifest as of its last download and its ETag: a pass asks for it only if it changed
    // (If-None-Match) and on "304 Not Modified" uses this copy. Kept in cache/skin_hashes/<skin>.manifest:
    // the URL, the ETag and the manifest's SHA-256 on a line each, then the manifest.
    struct SavedManifest {
        std::string etag; // empty: none saved
        std::string text;
    };

    fs::path manifest_cache_path(const fs::path& skin_dir) {
        return fs::path("cache") / "skin_hashes" / (skin_dir.filename().string() + ".manifest");
    }

    // nothing unless the saved manifest is the one of `url` and is whole
    SavedManifest load_manifest(const fs::path& skin_dir, const std::string& url) {
        std::ifstream in(manifest_cache_path(skin_dir), std::ios::binary);
        std::string saved_url, etag, hash;
        if (!std::getline(in, saved_url) || !std::getline(in, etag) || !std::getline(in, hash) || saved_url != url ||
            etag.empty()) {
            return {};
        }
        std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (text.empty() || text.back() != '\n' || crypto::to_hex(crypto::sha256(text)) != hash) {
            return {};
        }
        return {std::move(etag), std::move(text)};
    }

    void save_manifest(const fs::path& skin_dir, const std::string& url, const std::string& etag,
                       const std::string& text) {
        const fs::path file = manifest_cache_path(skin_dir);
        std::error_code ec;
        if (etag.empty() || etag.find_first_of("\r\n") != std::string::npos ||
            url.find_first_of("\r\n") != std::string::npos) {
            fs::remove(file, ec); // no ETag: the next pass downloads it again
            return;
        }
        const fs::path tmp = fs::path(file).concat(".tmp");
        fs::create_directories(file.parent_path(), ec);
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                return;
            }
            out << url << '\n' << etag << '\n' << crypto::to_hex(crypto::sha256(text)) << '\n' << text;
            if (!out) {
                return;
            }
        }
        fs::rename(tmp, file, ec);
        if (ec) {
            fs::remove(tmp, ec);
        }
    }

    // Size and modification time of `path`; false if it cannot be read
    bool file_stamp(const fs::path& path, uintmax_t& size, int64_t& mtime) {
        std::error_code ec;
        size = fs::file_size(path, ec);
        if (ec) {
            return false;
        }
        const auto t = fs::last_write_time(path, ec);
        if (ec) {
            return false;
        }
        mtime = static_cast<int64_t>(t.time_since_epoch().count());
        return true;
    }

    // HTTP 200 alone is not enough: a transfer cut short by the timeout still carries the 200 it
    // started with, and its body is only the part that arrived.
    bool fetched(const cpr::Response& r) {
        return r.status_code == 200 && r.error.code == cpr::ErrorCode::OK;
    }

    // Streams `url` to `sink` piece by piece as it arrives. Download() leaves the callback set on the
    // session, so every request of a session must go through here (it sets its own each time).
    cpr::Response fetch(cpr::Session& session, const std::string& url, int32_t timeout_ms,
                        const std::function<bool(std::string_view)>& sink) {
        session.SetUrl(cpr::Url{url});
        session.SetTimeout(cpr::Timeout{timeout_ms});
        session.SetConnectTimeout(cpr::ConnectTimeout{5000});
        session.SetLowSpeed(cpr::LowSpeed{1, 30}); // stalled for 30 s
        return session.Download(
            cpr::WriteCallback{[&sink](const std::string_view& data, intptr_t) { return sink(data); }});
    }

    // scheme://host[:port] of `url`
    std::string url_host(const std::string& url) {
        const size_t scheme = url.find("://");
        const size_t start = scheme == std::string::npos ? 0 : scheme + 3;
        return url.substr(0, url.find('/', start));
    }

    // A worker's sessions, one per host: curl keeps each one's connection open between requests, so
    // a pass makes one TLS handshake per worker and host instead of one per skin or file.
    struct Worker {
        std::unordered_map<std::string, std::unique_ptr<cpr::Session>> sessions;

        cpr::Session& session_for(const std::string& url) {
            std::unique_ptr<cpr::Session>& s = sessions[url_host(url)];
            if (!s) {
                s = std::make_unique<cpr::Session>();
                init_session(*s);
            }
            return *s;
        }
    };

    bool is_lfs_pointer(std::string_view body) {
        static constexpr std::string_view kLfsMarker = "version https://git-lfs.github.com/spec/v1";
        return body.starts_with(kLfsMarker);
    }

    // A download streamed into a file, hashed on the way: memory use doesn't grow with the file.
    struct FileFetch {
        cpr::Response resp;
        bool written = false; // every byte that arrived reached the file
        uintmax_t size = 0;
        std::string hash;
        std::string head; // the first bytes, enough to recognise an LFS pointer

        bool complete() const { return written && fetched(resp); }
        bool lfs_pointer() const { return size <= 1024 && ::is_lfs_pointer(head); }
    };

    FileFetch fetch_to_file(cpr::Session& session, const std::string& url, int32_t timeout_ms, const fs::path& file) {
        constexpr size_t kHeadBytes = 64;
        FileFetch got;
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) {
            return got;
        }
        crypto::Sha256 ctx;
        got.resp = fetch(session, url, timeout_ms, [&](std::string_view data) {
            out.write(data.data(), static_cast<std::streamsize>(data.size()));
            if (!out) {
                return false; // aborts the transfer
            }
            ctx.update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
            if (got.head.size() < kHeadBytes) {
                got.head.append(data.substr(0, kHeadBytes - got.head.size()));
            }
            got.size += data.size();
            return true;
        });
        out.close();
        got.written = static_cast<bool>(out);
        got.hash = crypto::to_hex(ctx.finalize());
        return got;
    }

    // Downloads are written to a file in <skin>/.skin-update-tmp and moved over the target only once
    // complete, so a pass cut short (the game killed, a full disk) leaves the old file, never a
    // half-written one. The folder is emptied at the start and end of each skin's pass.
    constexpr const char* kTempDirName = ".skin-update-tmp";
    // Z-DON: a local edit replaced by an upstream change is copied here first, as <rel>.<timestamp>
    constexpr const char* kBackupDirName = ".zdon-local-backup";

    fs::path temp_dir(const fs::path& skin_dir) {
        return skin_dir / kTempDirName;
    }

    // the updater's own folders, never a skin file even if a manifest listed one
    bool is_updater_path(const fs::path& rel) {
        return !rel.empty() && (*rel.begin() == kTempDirName || *rel.begin() == kBackupDirName);
    }

    // Renames the finished temp file over `target` (std::filesystem::rename replaces an existing file,
    // on Windows too: MoveFileExW with MOVEFILE_REPLACE_EXISTING). On failure `target` is untouched.
    bool replace_file(const fs::path& tmp, const fs::path& target) {
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        fs::rename(tmp, target, ec);
        return !ec;
    }

} // namespace

void SkinUpdater::set_current_skin(const std::string& name) {
    std::scoped_lock lock(status_mutex_);
    current_skin_ = name;
    current_file_.clear();
}

void SkinUpdater::set_current_file(const std::string& skin, const std::string& file) {
    std::scoped_lock lock(status_mutex_);
    current_skin_ = skin;
    current_file_ = file;
}

void SkinUpdater::note_file_updated() {
    std::scoped_lock lock(status_mutex_);
    ++files_updated_;
}

void SkinUpdater::set_progress(const std::string& skin, int done, int total) {
    std::scoped_lock lock(status_mutex_);
    progress_[skin] = {done, total};
}

// One pass over every skin: a check task per skin (manifest, local hashes), then a download task per
// file that is missing or differs, all run by a few background threads. Whichever task finishes a
// skin's last download does its removals and saves its hash cache.
class SkinUpdatePass {
public:
    // keep_local: Z-DON's three-way rule (ZDON_SKIN_UPDATE_KEEP_LOCAL=0 turns it off)
    SkinUpdatePass(SkinUpdater& updater, bool keep_local)
        : u_(updater), keep_local_(keep_local), timestamp_(timestamp_now()) {}

    void add_skin(const fs::path& dir, const std::string& repo_url, const std::string& branch) {
        auto skin = std::make_shared<Skin>();
        skin->dir = dir;
        skin->name = dir.filename().string();
        skin->repo_url = repo_url;
        skin->branch = branch;
        push([this, skin](Worker& w) { check(w, skin); });
    }

    // runs the queued checks and everything they queue; returns once all are done
    void run(int workers) {
        std::vector<std::thread> pool;
        for (int i = 0; i < workers && pending_ > 0; i++) {
            pool.emplace_back([this] { worker_loop(); });
        }
        for (std::thread& t : pool) {
            t.join();
        }
    }

private:
    struct Job {
        std::string rel_path, hash; // rel_path: the manifest's own spelling (UTF-8), the cache key
        fs::path local_file;
        bool backup = false; // copy the local file to the backup folder before replacing it
    };
    struct Skin {
        fs::path dir;
        std::string name, repo_url, branch;
        HashCache cache; // as loaded; read-only once the downloads start
        std::unordered_set<std::string> listed;
        std::vector<Job> jobs; // fixed once the downloads start
        std::mutex mutex;      // guards seen, bases_out, updated
        HashCache seen;        // this manifest's files: entries for files it no longer lists are dropped
        BaseMap bases;         // as loaded
        BaseMap bases_out;     // this manifest's files
        int updated = 0;
        std::atomic<int> done{0};   // downloads finished (written or failed)
        std::atomic<int> serial{0}; // temp file names

        std::string raw_url(const std::string& rel) const { return repo_url + "/raw/branch/" + branch + "/" + rel; }
        std::string media_url(const std::string& rel) const { return repo_url + "/media/branch/" + branch + "/" + rel; }
    };
    using Task = std::function<void(Worker&)>;

    void push(Task task) {
        {
            std::scoped_lock lock(mutex_);
            queue_.push_back(std::move(task));
            ++pending_;
        }
        cv_.notify_one();
    }

    void worker_loop() {
#ifdef _WIN32
        win32_lower_thread_priority(); // below the game, like the updater thread
#endif
        Worker w; // its sessions live as long as the pass
        for (;;) {
            Task task;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return !queue_.empty() || pending_ == 0; });
                if (queue_.empty()) {
                    return; // nothing queued or running: the pass is done
                }
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                task(w);
            } catch (const std::exception& e) {
                spdlog::error("Skin update: unhandled exception: {}", e.what());
            }
            bool all_done = false;
            {
                std::scoped_lock lock(mutex_);
                all_done = --pending_ == 0;
            }
            if (all_done) {
                cv_.notify_all();
            }
        }
    }

    void check(Worker& w, const std::shared_ptr<Skin>& skin) {
        const fs::path& skin_dir = skin->dir;
        u_.set_current_skin(skin->name);
        std::error_code tmp_ec;
        fs::remove_all(temp_dir(skin_dir), tmp_ec); // left by a pass that was cut short

        // a whole manifest or nothing: files it doesn't list get removed below, so a truncated
        // one would delete everything past the cut (and the last line must be complete)
        const std::string manifest_url = skin->raw_url("checksums.sha256");
        cpr::Session& session = w.session_for(manifest_url);
        SavedManifest saved = load_manifest(skin_dir, manifest_url);
        if (!saved.etag.empty()) {
            session.SetHeader(cpr::Header{{"If-None-Match", saved.etag}});
        }
        std::string body;
        cpr::Response checksums = fetch(session, manifest_url, 60000, [&body](std::string_view data) {
            body.append(data);
            return true;
        });
        session.SetHeader(cpr::Header{});
        std::string manifest;
        if (!saved.etag.empty() && checksums.status_code == 304 && checksums.error.code == cpr::ErrorCode::OK) {
            manifest = std::move(saved.text); // unchanged since the last pass
        } else if (fetched(checksums) && !body.empty() && body.back() == '\n') {
            manifest = std::move(body);
            auto etag = checksums.header.find("ETag");
            save_manifest(skin_dir, manifest_url, etag != checksums.header.end() ? etag->second : std::string(),
                          manifest);
        } else {
            spdlog::warn("Skin update ({}): could not fetch a complete checksums.sha256 (HTTP {}, curl error {}: {})",
                         skin->name, checksums.status_code, static_cast<int>(checksums.error.code),
                         checksums.error.message);
            return;
        }

        skin->cache = load_hash_cache(skin_dir);
        skin->bases = load_bases(skin_dir);
        const HashCache& cache = skin->cache;
        HashCache& seen = skin->seen; // no download runs yet: no lock needed
        std::istringstream lines(manifest);
        std::string hash, rel_path;
        while (lines >> hash >> rel_path) {
            if (!rel_path.empty() && rel_path.front() == '*') {
                rel_path.erase(0, 1);
            }
            if (fs::path(rel_path).filename() == "checksums.sha256") {
                continue;
            }

            fs::path local_file = (skin_dir / rel_path).lexically_normal();
            const fs::path local_rel = local_file.lexically_relative(skin_dir);
            if (local_rel.empty() || *local_rel.begin() == ".." || is_updater_path(local_rel)) {
                spdlog::warn("Skin update ({}): skipping unsafe manifest path {}", skin->name, rel_path);
                continue;
            }
            const std::string& key = rel_path;
            skin->listed.insert(key);
            uintmax_t size = 0;
            int64_t mtime = 0;
            bool backup = false;
            const auto base = skin->bases.find(key);
            if (file_stamp(local_file, size, mtime)) {
                // unchanged since its last check: the cached hash stands for the file
                auto cached = cache.find(key);
                std::string local_hash;
                const bool unchanged_since_check =
                    cached != cache.end() && cached->second.size == size && cached->second.mtime == mtime;
                if (unchanged_since_check) {
                    local_hash = cached->second.hash;
                } else {
                    local_hash = sha256_file(local_file);
                }
                if (!local_hash.empty()) {
                    seen[key] = HashCacheEntry{local_hash, size, mtime};
                }
                if (local_hash == hash) {
                    skin->bases_out[key] = BaseEntry{hash, local_hash};
                    continue;
                }
                if (keep_local_ && !local_hash.empty()) {
                    if (base != skin->bases.end() && base->second.manifest_hash == hash) {
                        // upstream hasn't changed it since it was installed: what differs is local
                        // (an edit; or, when it is still what was installed, a manifest that is
                        // behind the repo): keep it, and don't count it as an update
                        if (local_hash != base->second.installed_hash) {
                            spdlog::info("Skin update ({}): keeping local edit of {} (unchanged upstream)", skin->name,
                                         rel_path);
                        }
                        skin->bases_out[key] = base->second;
                        continue;
                    }
                    // Upstream changed it (or no base is known yet): replace it, keeping a copy if it
                    // was edited here. Without a base, "edited" = changed since the updater last
                    // recorded it (or never recorded), so the first upstream update after this
                    // version doesn't back up every untouched file.
                    backup =
                        base != skin->bases.end() ? local_hash != base->second.installed_hash : !unchanged_since_check;
                }
            }
            if (base != skin->bases.end()) {
                skin->bases_out[key] = base->second; // until the download replaces it
            }

            // File is missing or differs: this pass has at least one real update.
            u_.any_updated_.store(true);
            skin->jobs.push_back({rel_path, hash, std::move(local_file), backup});
        }

        if (skin->jobs.empty()) {
            finish(*skin);
            return;
        }
        const int total = static_cast<int>(skin->jobs.size());
        u_.set_progress(skin->name, 0, total);
        for (size_t i = 0; i < skin->jobs.size(); i++) {
            push([this, skin, i](Worker& worker) { download(worker, skin, i); });
        }
    }

    void download(Worker& w, const std::shared_ptr<Skin>& skin, size_t i) {
        try {
            download_file(w, *skin, skin->jobs[i]);
        } catch (const std::exception& e) {
            spdlog::error("Skin update ({}): {}: {}", skin->name, skin->jobs[i].rel_path, e.what());
        }
        const int total = static_cast<int>(skin->jobs.size());
        const int done = ++skin->done;
        u_.set_progress(skin->name, done, total);
        if (done == total) {
            finish(*skin);
        }
    }

    void download_file(Worker& w, Skin& skin, const Job& job) {
        const std::string& rel_path = job.rel_path;
        u_.set_current_file(skin.name, rel_path);

        std::error_code ec;
        fs::create_directories(temp_dir(skin.dir), ec);
        const fs::path tmp = temp_dir(skin.dir) / (std::to_string(skin.serial++) + ".tmp");
        auto discard = [&tmp] {
            std::error_code rm_ec;
            fs::remove(tmp, rm_ec);
        };

        const std::string url = skin.raw_url(rel_path);
        FileFetch got = fetch_to_file(w.session_for(url), url, 60000, tmp);
        if (!got.complete()) {
            spdlog::warn("Skin update ({}): failed to download {} (HTTP {})", skin.name, rel_path,
                         got.resp.status_code);
            discard();
            return;
        }

        if (got.lfs_pointer()) {
            spdlog::debug("Skin update ({}): {} is an LFS pointer; fetching content from media endpoint", skin.name,
                          rel_path);
            const std::string lfs_url = skin.media_url(rel_path);
            got = fetch_to_file(w.session_for(lfs_url), lfs_url, 120000, tmp);
            if (!got.complete()) {
                spdlog::warn("Skin update ({}): failed to download LFS content {} (HTTP {})", skin.name, rel_path,
                             got.resp.status_code);
                discard();
                return;
            }
        }

        // A complete download is written even when it doesn't match the manifest: that only means
        // the manifest is behind the repo, and keeping the old file (or none, for a new one) would
        // leave the skin half updated.
        if (got.hash != job.hash) {
            spdlog::debug("Skin update ({}): {} differs from its checksums.sha256 entry (stale manifest)", skin.name,
                          rel_path);
        }

        if (job.backup && !back_up(skin, job)) {
            discard();
            return;
        }
        if (!replace_file(tmp, job.local_file)) {
            spdlog::warn("Skin update ({}): failed to write {}; keeping the old file", skin.name, rel_path);
            discard();
            return;
        }
        uintmax_t size = 0;
        int64_t mtime = 0;
        const bool stamped = file_stamp(job.local_file, size, mtime);
        {
            std::scoped_lock lock(skin.mutex);
            if (stamped) {
                skin.seen[rel_path] = HashCacheEntry{got.hash, size, mtime};
            } else {
                skin.seen.erase(rel_path);
            }
            skin.bases_out[rel_path] = BaseEntry{job.hash, got.hash};
            ++skin.updated;
        }
        u_.note_file_updated();
    }

    // Copies the local file to <skin>/.zdon-local-backup/<rel>.<timestamp>; false (nothing replaced)
    // if that fails
    bool back_up(const Skin& skin, const Job& job) {
        std::error_code ec;
        if (!fs::exists(job.local_file, ec)) {
            return true;
        }
        const fs::path rel = job.local_file.lexically_relative(skin.dir);
        fs::path dest = skin.dir / kBackupDirName / fs::path(rel).concat("." + timestamp_);
        for (int n = 2; fs::exists(dest, ec) && n < 100; n++) {
            dest = skin.dir / kBackupDirName / fs::path(rel).concat("." + timestamp_ + "-" + std::to_string(n));
        }
        fs::create_directories(dest.parent_path(), ec);
        if (!fs::copy_file(job.local_file, dest, ec) || ec) {
            spdlog::warn("Skin update ({}): could not back up local edit of {} ({}); keeping it", skin.name,
                         job.rel_path, ec.message());
            return false;
        }
        spdlog::info("Skin update ({}): {} was edited locally and changed upstream; local copy saved to {}", skin.name,
                     job.rel_path, dest.generic_string());
        return true;
    }

    // after the skin's last download: removes the files the manifest dropped, saves the hash cache
    void finish(Skin& skin) {
        const fs::path& skin_dir = skin.dir;
        int removed = 0;
        for (const auto& [rel, e] : skin.cache) {
            if (skin.listed.contains(rel)) {
                continue;
            }
            const fs::path local_file = (skin_dir / rel).lexically_normal();
            const fs::path local_rel = local_file.lexically_relative(skin_dir);
            if (local_rel.empty() || *local_rel.begin() == "..") {
                continue;
            }
            uintmax_t size = 0;
            int64_t mtime = 0;
            if (!file_stamp(local_file, size, mtime) || size != e.size || mtime != e.mtime) {
                continue;
            }
            // Z-DON: nor if what was recorded is a local edit (not what the updater installed)
            if (const auto base = skin.bases.find(rel);
                keep_local_ && base != skin.bases.end() && e.hash != base->second.installed_hash) {
                spdlog::info("Skin update ({}): keeping local edit of {} (removed upstream)", skin.name, rel);
                continue;
            }
            std::error_code ec;
            if (!fs::remove(local_file, ec)) {
                continue;
            }
            ++removed;
            for (fs::path dir = local_file.parent_path(); dir != skin_dir && fs::is_empty(dir, ec) && !ec;
                 dir = dir.parent_path()) {
                if (!fs::remove(dir, ec)) {
                    break;
                }
            }
        }
        if (removed > 0) {
            u_.any_updated_.store(true);
        }

        std::scoped_lock lock(skin.mutex);
        if (skin.seen != skin.cache) {
            save_hash_cache(skin_dir, skin.seen);
        }
        if (skin.bases_out != skin.bases) {
            save_bases(skin_dir, skin.bases_out);
        }
        std::error_code tmp_ec;
        fs::remove_all(temp_dir(skin_dir), tmp_ec);
        spdlog::info("Skin update ({}): {} file(s) updated, {} removed", skin.name, skin.updated, removed);
    }

    SkinUpdater& u_;
    const bool keep_local_;
    const std::string timestamp_; // of the pass, for backup names
    std::mutex mutex_;            // guards queue_, pending_
    std::condition_variable cv_;
    std::deque<Task> queue_;
    int pending_ = 0; // tasks queued or running
};

void SkinUpdater::scan_skins() {
    std::error_code ec;
    if (!fs::exists("Skins", ec)) {
        return;
    }
    // dev hooks: ZDON_SKIN_UPDATE_ONLY=<skin folder>[,<skin folder>...] checks only those skins (tests that must not
    // reach the real servers); ZDON_SKIN_UPDATE_THREADS=<n> (1..16, default 8) downloads/checks at once
    const char* only = std::getenv("ZDON_SKIN_UPDATE_ONLY");
    int workers = 8;
    if (const char* n = std::getenv("ZDON_SKIN_UPDATE_THREADS")) {
        workers = std::clamp(std::atoi(n), 1, 16);
    }

    // ZDON_SKIN_UPDATE_KEEP_LOCAL=0: overwrite local edits like upstream (no keep, no backups)
    const char* keep = std::getenv("ZDON_SKIN_UPDATE_KEEP_LOCAL");
    const bool keep_local = keep == nullptr || std::string_view(keep) != "0";

    SkinUpdatePass pass(*this, keep_local);
    for (const auto& entry : fs::directory_iterator("Skins", ec)) {
        if (ec || !entry.is_directory()) {
            continue;
        }
        if (only != nullptr &&
            !std::ranges::contains(std::views::split(std::string_view(only), ','), entry.path().filename().string(),
                                   [](auto part) { return std::string_view(part.begin(), part.end()); })) {
            continue;
        }

        std::ifstream repo_file(entry.path() / ".skin-repo");
        if (!repo_file) {
            continue;
        }
        std::string repo_url, branch;
        std::getline(repo_file, repo_url);
        std::getline(repo_file, branch);
        trim(repo_url);
        trim(branch);
        if (repo_url.empty()) {
            continue;
        }
        if (branch.empty()) {
            branch = "main";
        }

        pass.add_skin(entry.path(), strip_dot_git(repo_url), branch);
    }
    const auto t0 = std::chrono::steady_clock::now();
    pass.run(workers);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    spdlog::info("Skin update: pass done in {} ms ({} worker(s))", ms, workers);
}

void SkinUpdater::start() {
    if (started_.exchange(true)) {
        return;
    }
    running_ = true;
    thread_ = std::thread([this] {
#ifdef _WIN32
        // below the game for CPU time: a pass without a hash cache reads every skin file
        win32_lower_thread_priority();
#endif
        try {
            scan_skins();
        } catch (const std::exception& e) {
            spdlog::error("Skin update: unhandled exception: {}", e.what());
        }
        running_ = false;
        std::scoped_lock lock(status_mutex_);
        current_skin_.clear();
        current_file_.clear();
        progress_.clear();
    });
}

bool SkinUpdater::finished() {
    return started_.load() && !running_.load();
}

bool SkinUpdater::applying_updates() {
    return running_.load() && any_updated_.load();
}

bool SkinUpdater::updated_this_pass() {
    return any_updated_.load();
}

SkinUpdater::Status SkinUpdater::snapshot() {
    Status s;
    s.running = running_.load();
    s.finished = finished();
    s.any_updated = any_updated_.load();
    std::scoped_lock lock(status_mutex_);
    s.current_skin = current_skin_;
    s.current_file = current_file_;
    s.files_updated = files_updated_;
    if (auto p = progress_.find(current_skin_); p != progress_.end()) {
        s.files_done = p->second.first;
        s.files_total = p->second.second;
    }
    return s;
}

void SkinUpdater::shutdown() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

SkinUpdater skin_updater;

#else // __EMSCRIPTEN__

void SkinUpdater::start() {}
bool SkinUpdater::finished() {
    return true;
}
bool SkinUpdater::applying_updates() {
    return false;
}
bool SkinUpdater::updated_this_pass() {
    return false;
}
SkinUpdater::Status SkinUpdater::snapshot() {
    Status s;
    s.finished = true;
    return s;
}
void SkinUpdater::shutdown() {}

SkinUpdater skin_updater;

#endif
