#include "readahead.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include "file/filesystem.h"
#include "global_data.h"
#include "texture_bc7.h"
#include "texture_pack.h"
#include "texture_retain.h"
#include "zdon_os.h" // not <fcntl.h> here: glibc's readahead() would clash with this namespace

namespace fs = std::filesystem;

namespace zdon_readahead { // not "readahead": glibc declares a readahead() function
    namespace {

        // Where a screen usually leads, by screen name (Graphics/<name>, Sounds/<name>).
        const std::unordered_map<std::string, std::vector<std::string>>& next_screens() {
            static const std::unordered_map<std::string, std::vector<std::string>> map = {
                {"title", {"entry", "song_select"}},    {"entry", {"song_select"}}, {"song_select", {"game", "result"}},
                {"dan_select", {"game", "dan_result"}}, {"game", {"result"}},       {"result", {"song_select"}},
                {"dan_result", {"dan_select"}},         {"retry_game", {"game"}},
            };
            return map;
        }

        // a cap per request, so a large skin can't push the whole page cache out
        constexpr uintmax_t kMaxBytes = uintmax_t{512} << 20;

        struct Request {
            fs::path chart;              // prepare_song: the chart, read with its folder before the screens
            std::vector<fs::path> roots; // the skin's root, then its ancestors'
            std::vector<std::string> screens;
            std::vector<std::string> resident; // screens whose textures are still loaded: Sounds only
        };

        std::mutex mutex;
        std::condition_variable cv;
        std::optional<Request> queued; // the latest request, taken by the thread
        uint64_t generation = 0;       // bumped by every request; the thread drops older work
        bool quit = false;

        void shut_down();

        // joined at exit too, so a read still running when the game quits doesn't terminate it
        struct Worker {
            std::thread t;
            ~Worker() { shut_down(); }
        } worker;

        bool superseded(uint64_t gen) {
            std::scoped_lock lock(mutex);
            return quit || generation != gen;
        }

        // Brings one file into the page cache; false when a newer request (or quit) came in.
        bool warm(const fs::path& path, uint64_t gen) {
            if (zdon_os::prefetch_file(path)) {
                return true; // the kernel reads it in asynchronously (Linux, Android, macOS, iOS)
            }
            std::ifstream in(path, std::ios::binary);
            static thread_local std::vector<char> buf(1 << 20);
            while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) || in.gcount() > 0) {
                if (superseded(gen)) {
                    return false;
                }
            }
            return true;
        }

        // How many files are read at once. A hard drive serves several outstanding reads faster than
        // one at a time (it reorders them, NCQ), so the next screen is warm sooner.
        // YATAIDON_READAHEAD_THREADS=<n> overrides it (1 = one file at a time).
        size_t reader_count() {
            static const size_t n = [] {
                const char* v = std::getenv("YATAIDON_READAHEAD_THREADS");
                const int k = v ? std::atoi(v) : 0;
                return k > 0 ? static_cast<size_t>(std::min(k, 32)) : size_t{8};
            }();
            return n;
        }

        // Lists the request's files (in the order the screens are likely to come), then reads them in,
        // several at a time; stops early for a newer request.
        void serve(const Request& req, uint64_t gen) {
            std::vector<fs::path> files;
            uintmax_t total = 0;
            if (!req.chart.empty()) {
                // the song's own folder, not recursive; of a folder of many loose charts only a few files
                constexpr size_t kMaxSongFiles = 16;
                std::error_code ec;
                files.push_back(req.chart);
                for (fs::directory_iterator it(req.chart.parent_path(), ec), end;
                     !ec && it != end && files.size() < kMaxSongFiles; it.increment(ec)) {
                    if (superseded(gen)) {
                        return;
                    }
                    std::error_code fec;
                    if (it->path() == req.chart || !it->is_regular_file(fec)) {
                        continue;
                    }
                    const uintmax_t size = it->file_size(fec);
                    if (fec || total + size > kMaxBytes) {
                        continue;
                    }
                    total += size;
                    files.push_back(it->path());
                }
            }
            const bool bc7_warm = texture_bc7::file_count() > 0;
            for (const std::string& screen : req.screens) {
                // A texture pack holds the screen's PNGs in one file: read that instead of the loose
                // PNGs (a few hundred small reads, parent files the child overrides among them).
                bool packed = false;
                // Z-DON: with the BC7 cache read, loads take the cached blocks and leave the pack unread
                // (texture_pack::Session), so neither the pack nor the PNGs are worth reading ahead
                if (bc7_warm && std::ranges::find(req.resident, screen) == req.resident.end()) {
                    packed = true;
                } else if (texture_pack::enabled() && !req.roots.empty() &&
                           std::ranges::find(req.resident, screen) == req.resident.end()) {
                    const fs::path pack = texture_pack::pack_path(req.roots.front().filename(), screen);
                    std::error_code ec;
                    const uintmax_t size = fs::file_size(pack, ec);
                    if (!ec && total + size <= kMaxBytes) {
                        total += size;
                        files.push_back(pack);
                        packed = true;
                    }
                }
                for (const fs::path& root : req.roots) {
                    for (const char* kind : {"Graphics", "Sounds"}) {
                        if (kind[0] == 'G' && std::ranges::find(req.resident, screen) != req.resident.end()) {
                            continue;
                        }
                        const fs::path dir = root / kind / screen;
                        std::error_code ec;
                        if (!fs::is_directory(dir, ec)) {
                            continue;
                        }
                        for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                            if (superseded(gen)) {
                                return;
                            }
                            if (!it->is_regular_file(ec)) {
                                continue;
                            }
                            if (packed && kind[0] == 'G' && it->path().extension() == ".png") {
                                continue;
                            }
                            const uintmax_t size = it->file_size(ec);
                            if (ec || total + size > kMaxBytes) {
                                continue;
                            }
                            total += size;
                            files.push_back(it->path());
                        }
                    }
                }
            }
            std::atomic<size_t> next{0};
            auto read_files = [&] {
                for (size_t i; (i = next.fetch_add(1)) < files.size();) {
                    if (!warm(files[i], gen)) {
                        return;
                    }
                }
            };
            // this thread reads too
            const size_t helpers = files.empty() ? 0 : std::min(reader_count(), files.size()) - 1;
            std::vector<std::thread> pool;
            pool.reserve(helpers);
            for (size_t k = 0; k < helpers; ++k) {
                pool.emplace_back([&] {
                    zdon_os::background_thread();
                    read_files();
                });
            }
            read_files();
            for (std::thread& t : pool) {
                t.join();
            }
        }

        void run() {
            zdon_os::background_thread(); // low CPU and I/O priority, so the game's own reads go first
            while (true) {
                Request req;
                uint64_t gen;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [] { return quit || queued.has_value(); });
                    if (quit) {
                        return;
                    }
                    req = std::move(*queued);
                    queued.reset();
                    gen = generation;
                }
                serve(req, gen);
            }
        }

        void shut_down() {
            {
                std::scoped_lock lock(mutex);
                quit = true;
                ++generation;
            }
            cv.notify_all();
            if (worker.t.joinable()) {
                worker.t.join();
            }
        }

    } // namespace

    static void queue_request(const std::string& screen_name, const fs::path& chart) {
#ifdef __EMSCRIPTEN__
        (void)screen_name; // no threads, and the files are in memory already
        (void)chart;
        return;
#else
        // YATAIDON_NO_READAHEAD=1 turns it off (A/B timing on a cold disk)
        static const bool disabled = [] {
            const char* v = std::getenv("YATAIDON_NO_READAHEAD");
            return v && *v == '1';
        }();
        if (disabled) {
            return;
        }
        const auto& map = next_screens();
        auto it = map.find(screen_name);

        std::scoped_lock lock(mutex);
        ++generation; // whatever the previous screen asked for is stale now
        queued.reset();
        if (it == map.end() || !global_data.config || quit) {
            return;
        }
        Request req;
        req.chart = chart;
        req.roots.push_back(fs::path("Skins") / global_data.config->paths.skin);
        for (const fs::path& r : skin_ancestor_roots()) {
            req.roots.push_back(r);
        }
        req.screens = it->second;
        for (const std::string& s : req.screens) {
            if (texture_retain::screen_complete(s)) {
                req.resident.push_back(s);
            }
        }
        queued = std::move(req);
        if (!worker.t.joinable()) {
            worker.t = std::thread(run);
        }
        cv.notify_one();
#endif
    }

    void after_screen(const std::string& screen_name) {
        queue_request(screen_name, {});
    }

    void prepare_song(const std::filesystem::path& chart) {
        queue_request("song_select", chart);
    }

    void cancel() {
        std::scoped_lock lock(mutex);
        ++generation;
        queued.reset();
    }

} // namespace zdon_readahead
