#include "texture_pack.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <spdlog/spdlog.h>

#include "texture_bc7.h"
#include "zdon_os.h"

namespace fs = std::filesystem;

namespace texture_pack {
    namespace {

        constexpr char kMagic[8] = {'Z', 'D', 'O', 'N', 'T', 'P', 'K', '1'};
        constexpr uint32_t kVersion = 1;
        constexpr size_t kHeaderBytes = 8 + 4 + 4 + 8 + 8 + 8;
        constexpr uint64_t kMaxIndexBytes = uint64_t{16} << 20;
        constexpr uint64_t kMaxPackBytes = uint64_t{512} << 20; // one screen's pack
        constexpr uint64_t kSpanGap = uint64_t{1} << 20;        // read through gaps up to this
        constexpr size_t kReadChunk = size_t{1} << 20;          // entries become usable per chunk
        constexpr uint64_t kMB = uint64_t{1} << 20;

        uint64_t hash64(const uint8_t* p, size_t n) {
            uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
            size_t i = 0;
            for (; i + 8 <= n; i += 8) {
                uint64_t w;
                std::memcpy(&w, p + i, 8);
                h = (h ^ w) * 0xFF51AFD7ED558CCDull;
                h ^= h >> 29;
            }
            uint64_t t = 0;
            std::memcpy(&t, p + i, n - i);
            h = (h ^ t) * 0xC4CEB9FE1A85EC53ull;
            h ^= h >> 32;
            return h;
        }

        void put_u32(std::string& s, uint32_t v) {
            s.append(reinterpret_cast<const char*>(&v), 4);
        }
        void put_u64(std::string& s, uint64_t v) {
            s.append(reinterpret_cast<const char*>(&v), 8);
        }

        struct Reader {
            const uint8_t* p;
            size_t n;
            size_t pos = 0;
            bool ok = true;
            template<class T>
            T get() {
                T v{};
                if (pos + sizeof(T) > n) {
                    ok = false;
                    return v;
                }
                std::memcpy(&v, p + pos, sizeof(T));
                pos += sizeof(T);
                return v;
            }
        };

        uint64_t cache_limit_bytes() {
            static const uint64_t limit = [] {
                if (const char* v = std::getenv("YATAIDON_TEXPACK_MB"); v && *v) {
                    const long mb = std::strtol(v, nullptr, 10);
                    return mb > 0 ? static_cast<uint64_t>(mb) * kMB : uint64_t{0};
                }
                return static_cast<uint64_t>(zdon_os::mobile_cap_mb(1024, 512)) * kMB;
            }();
            return limit;
        }

        // ---- background writer ----

        struct WriteItem {
            std::string key;
            FileStat stat;
            uint64_t hash = 0;
            bool hashed = false;
            std::shared_ptr<const void> owner; // keeps data alive
            const uint8_t* data = nullptr;     // nullptr: copy from the old pack at old_offset
            uint64_t old_offset = 0;
        };
        struct Job {
            fs::path path;
            std::string screen;
            uint64_t old_payload_start = 0;
            std::vector<WriteItem> items;
        };

        class Writer {
        public:
            void push(Job job) {
                std::scoped_lock lock(mutex);
                if (quit) {
                    return;
                }
                // a newer job for the same pack replaces one not started yet
                for (Job& j : queue) {
                    if (j.path == job.path) {
                        j = std::move(job);
                        cv.notify_one();
                        return;
                    }
                }
                queue.push_back(std::move(job));
                if (!thread.joinable()) {
                    thread = std::thread([this] { run(); });
                }
                cv.notify_one();
            }

            ~Writer() {
                {
                    std::scoped_lock lock(mutex);
                    quit = true;
                    queue.clear();
                }
                cv.notify_all();
                if (thread.joinable()) {
                    thread.join();
                }
            }

            bool quitting() const { return quit.load(); }

        private:
            void run() {
                zdon_os::background_thread(); // low CPU and I/O priority: the game's reads go first
                while (true) {
                    Job job;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        cv.wait(lock, [this] { return quit || !queue.empty(); });
                        if (quit) {
                            return;
                        }
                        job = std::move(queue.front());
                        queue.pop_front();
                    }
                    try {
                        write(job);
                    } catch (const std::exception& e) {
                        if (!quit) {
                            spdlog::warn("Texture pack {}: not written ({})", job.path.string(), e.what());
                        }
                    }
                }
            }

            void write(Job& job) {
                const auto t0 = std::chrono::steady_clock::now();
                {
                    // the entries kept from the old pack
                    std::ifstream old;
                    for (WriteItem& it : job.items) {
                        if (it.data) {
                            continue;
                        }
                        if (!old.is_open()) {
                            old.open(job.path, std::ios::binary);
                        }
                        auto buf = std::make_shared<std::vector<uint8_t>>(it.stat.size);
                        old.seekg(static_cast<std::streamoff>(job.old_payload_start + it.old_offset));
                        if (!old.read(reinterpret_cast<char*>(buf->data()),
                                      static_cast<std::streamsize>(buf->size()))) {
                            old.clear();
                            it.key.clear(); // drop it
                            continue;
                        }
                        it.data = buf->data();
                        it.owner = buf;
                        if (quit) {
                            return;
                        }
                    }
                }
                std::vector<WriteItem*> items;
                uint64_t payload = 0;
                for (WriteItem& it : job.items) {
                    if (it.key.empty() || !it.data) {
                        continue;
                    }
                    const uint64_t h = hash64(it.data, it.stat.size);
                    if (it.hashed && h != it.hash) {
                        continue; // a copy that went bad on the way
                    }
                    it.hash = h;
                    items.push_back(&it);
                    payload += it.stat.size;
                }
                std::string index;
                uint64_t offset = 0;
                for (const WriteItem* it : items) {
                    put_u32(index, static_cast<uint32_t>(it->key.size()));
                    index += it->key;
                    put_u64(index, it->stat.size);
                    put_u64(index, it->stat.mtime);
                    put_u64(index, offset);
                    put_u64(index, it->hash);
                    offset += it->stat.size;
                }
                std::string header(kMagic, sizeof(kMagic));
                put_u32(header, kVersion);
                put_u32(header, static_cast<uint32_t>(items.size()));
                put_u64(header, index.size());
                put_u64(header, payload);
                put_u64(header, hash64(reinterpret_cast<const uint8_t*>(index.data()), index.size()));

                std::error_code ec;
                fs::create_directories(job.path.parent_path(), ec);
                fs::path tmp = job.path;
                tmp += ".tmp";
                bool ok = false;
                {
                    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                    out.write(header.data(), static_cast<std::streamsize>(header.size()));
                    out.write(index.data(), static_cast<std::streamsize>(index.size()));
                    for (const WriteItem* it : items) {
                        if (quit) {
                            break;
                        }
                        out.write(reinterpret_cast<const char*>(it->data), static_cast<std::streamsize>(it->stat.size));
                    }
                    out.flush();
                    ok = out.good() && !quit;
                }
                const size_t written = items.size();
                items.clear();
                job.items.clear(); // let the memory go before the rename and the cache trim
                if (ok) {
                    fs::rename(tmp, job.path, ec);
                    ok = !ec;
                }
                if (!ok) {
                    fs::remove(tmp, ec);
                    if (!quit) {
                        spdlog::warn("Texture pack {}: not written", job.path.string());
                    }
                    return;
                }
                const uint64_t total = trim(job.path);
                if (quit) {
                    return;
                }
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                spdlog::info(
                    "Texture pack written for {}: {} files, {:.1f} MB in {:.0f} ms (pack cache {:.1f} of {} MB)",
                    job.screen, written, static_cast<double>(payload) / kMB, ms, static_cast<double>(total) / kMB,
                    cache_limit_bytes() / kMB);
            }

            // Keeps cache/texpack within the limit, dropping the least recently written packs (never
            // keep); returns the bytes left.
            static uint64_t trim(const fs::path& keep) {
                struct File {
                    fs::path path;
                    uint64_t size;
                    fs::file_time_type time;
                };
                std::vector<File> files;
                uint64_t total = 0;
                std::error_code ec;
                const fs::path root = keep.parent_path().parent_path();
                for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
                    std::error_code fec;
                    if (!it->is_regular_file(fec)) {
                        continue;
                    }
                    const uint64_t size = it->file_size(fec);
                    if (fec) {
                        continue;
                    }
                    total += size;
                    files.push_back({it->path(), size, it->last_write_time(fec)});
                }
                std::ranges::sort(files, [](const File& a, const File& b) { return a.time < b.time; });
                for (const File& f : files) {
                    if (total <= cache_limit_bytes()) {
                        break;
                    }
                    if (f.path == keep) {
                        continue;
                    }
                    std::error_code rec;
                    if (fs::remove(f.path, rec)) {
                        total -= f.size;
                    }
                }
                return total;
            }

            std::mutex mutex;
            std::condition_variable cv;
            std::deque<Job> queue;
            std::thread thread;
            std::atomic<bool> quit{false};
        };

        Writer& writer() {
            static Writer w; // joined at exit, like readahead's thread
            return w;
        }

    } // namespace

    struct Session::Span {
        uint64_t begin = 0;
        uint64_t end = 0;
        std::unique_ptr<uint8_t[]> buf;
        size_t ready = 0; // bytes read so far (guarded by the session's mutex)
        bool failed = false;
    };

    bool enabled() {
#if defined(__EMSCRIPTEN__)
        return false;
#else
        // Android/iOS: only on a device with the memory for it (zdon_os), unless YATAIDON_TEXPACK says
        static const bool on = [] {
            const char* v = std::getenv("YATAIDON_TEXPACK");
            return v && *v ? v[0] != '0' : zdon_os::texture_extras_by_default();
        }();
        return on;
#endif
    }

    fs::path pack_path(const fs::path& skin, const std::string& screen) {
        return fs::path("cache") / "texpack" / skin / (screen + ".pack");
    }

    namespace {
        // YATAIDON_TEXPACK_VERIFY=1: every file goes through the pack (texture.cpp compares it with the skin's)
        bool verify_mode() {
            static const bool on = [] {
                const char* v = std::getenv("YATAIDON_TEXPACK_VERIFY");
                return v && v[0] == '1';
            }();
            return on;
        }
    } // namespace

    bool read_file(const fs::path& path, std::vector<uint8_t>& bytes) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) {
            return false;
        }
        const std::streamoff n = in.tellg();
        if (n <= 0) {
            return false;
        }
        bytes.resize(static_cast<size_t>(n));
        in.seekg(0);
        return static_cast<bool>(in.read(reinterpret_cast<char*>(bytes.data()), n));
    }

    Session::Session(fs::path pack, std::string screen_name, std::vector<Planned> planned, Peek peek, void* peek_ctx,
                     bool background)
        : path(std::move(pack)), screen(std::move(screen_name)) {
        entries.reserve(planned.size());
        for (Planned& p : planned) {
            if (by_key.contains(p.key)) {
                continue;
            }
            by_key.emplace(p.key, entries.size());
            Entry e;
            e.key = std::move(p.key);
            e.stat = p.stat;
            e.planned = true;
            entries.push_back(std::move(e));
        }
        try {
            read_index();
        } catch (const std::exception&) {
            pack_ok = false;
        }
        if (!pack_ok) {
            return;
        }
        // entries the load did not plan: keep those that are still current (or can't be checked)
        for (auto& e : entries) {
            if (e.planned) {
                continue;
            }
            FileStat now;
            const int r = peek ? peek(peek_ctx, e.key, now) : -1;
            e.carry = r < 0 || (r == 1 && now.size == e.stat.size && now.mtime == e.stat.mtime);
            if (!e.carry) {
                dirty = true;
            }
        }
        // the hits, read in pack order, nearby ones in one go. Z-DON: not the files whose BC7 image the
        // cache holds -- the load takes that and never asks the pack (they are copied over on a rewrite)
        const bool bc7 = texture_bc7::enabled() && !verify_mode();
        std::vector<Entry*> hit_list;
        for (Entry& e : entries) {
            if (e.state == 1 && bc7 && texture_bc7::has_file(e.key, e.stat.size)) {
                e.state = 4;
                ++bc7_skipped;
            } else if (e.state == 1) {
                hit_list.push_back(&e);
            }
        }
        std::ranges::sort(hit_list, [](const Entry* a, const Entry* b) { return a->offset < b->offset; });
        for (Entry* e : hit_list) {
            if (spans.empty() || e->offset > spans.back()->end + kSpanGap) {
                auto s = std::make_shared<Span>();
                s->begin = s->end = e->offset;
                spans.push_back(std::move(s));
            }
            Span& s = *spans.back();
            s.end = std::max(s.end, e->offset + e->size);
            e->span = spans.back();
            e->span_offset = static_cast<size_t>(e->offset - s.begin);
        }
        hits = hit_list.size();
        if (!spans.empty()) {
            reader = std::thread([this, background] {
                if (background) {
                    zdon_os::background_thread();
                }
                read_spans();
            });
        }
    }

    void Session::read_index() {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) {
            return; // no pack yet
        }
        const uint64_t file_size = static_cast<uint64_t>(in.tellg());
        in.seekg(0);
        char head[kHeaderBytes];
        if (file_size < kHeaderBytes || !in.read(head, kHeaderBytes)) {
            dirty = true;
            return;
        }
        Reader h{reinterpret_cast<const uint8_t*>(head), kHeaderBytes};
        char magic[8];
        for (char& c : magic) {
            c = h.get<char>();
        }
        const uint32_t version = h.get<uint32_t>();
        const uint32_t count = h.get<uint32_t>();
        const uint64_t index_bytes = h.get<uint64_t>();
        const uint64_t payload_bytes = h.get<uint64_t>();
        const uint64_t index_hash = h.get<uint64_t>();
        if (std::memcmp(magic, kMagic, 8) != 0 || version != kVersion || index_bytes > kMaxIndexBytes ||
            payload_bytes > kMaxPackBytes || file_size != kHeaderBytes + index_bytes + payload_bytes) {
            dirty = true; // another version, or a partial file
            return;
        }
        std::vector<uint8_t> index(static_cast<size_t>(index_bytes));
        if (!in.read(reinterpret_cast<char*>(index.data()), static_cast<std::streamsize>(index.size())) ||
            hash64(index.data(), index.size()) != index_hash) {
            dirty = true;
            return;
        }
        Reader r{index.data(), index.size()};
        struct Old {
            std::string key;
            FileStat stat;
            uint64_t offset, hash;
        };
        std::vector<Old> old;
        old.reserve(count);
        for (uint32_t i = 0; i < count && r.ok; ++i) {
            const uint32_t len = r.get<uint32_t>();
            if (!r.ok || r.pos + len > r.n) {
                r.ok = false;
                break;
            }
            Old o;
            o.key.assign(reinterpret_cast<const char*>(index.data() + r.pos), len);
            r.pos += len;
            o.stat.size = r.get<uint64_t>();
            o.stat.mtime = r.get<uint64_t>();
            o.offset = r.get<uint64_t>();
            o.hash = r.get<uint64_t>();
            if (o.offset > payload_bytes || o.stat.size > payload_bytes - o.offset) {
                r.ok = false;
                break;
            }
            old.push_back(std::move(o));
        }
        if (!r.ok || r.pos != r.n) {
            dirty = true;
            return;
        }
        payload_start = kHeaderBytes + index_bytes;
        pack_ok = true;
        for (Old& o : old) {
            auto it = by_key.find(o.key);
            if (it != by_key.end()) {
                Entry& e = entries[it->second];
                if (!e.planned || e.state != 0) {
                    continue; // a duplicate key in the pack
                }
                e.offset = o.offset;
                e.size = o.stat.size;
                e.hash = o.hash;
                if (e.stat.size == o.stat.size && e.stat.mtime == o.stat.mtime) {
                    e.state = 1;
                } else {
                    dirty = true; // the file changed since the pack was written
                }
                continue;
            }
            by_key.emplace(o.key, entries.size());
            Entry e;
            e.key = std::move(o.key);
            e.stat = o.stat;
            e.offset = o.offset;
            e.size = o.stat.size;
            e.hash = o.hash;
            entries.push_back(std::move(e));
        }
    }

    void Session::read_spans() {
        std::ifstream in(path, std::ios::binary);
        for (const std::shared_ptr<Span>& sp : spans) {
            Span& s = *sp;
            const size_t n = static_cast<size_t>(s.end - s.begin);
            bool ok = static_cast<bool>(in);
            if (ok) {
                s.buf.reset(new (std::nothrow) uint8_t[n]);
                ok = s.buf != nullptr;
            }
            if (ok) {
                in.seekg(static_cast<std::streamoff>(payload_start + s.begin));
            }
            size_t done = 0;
            while (ok && done < n) {
                const size_t k = std::min(kReadChunk, n - done);
                ok = static_cast<bool>(
                    in.read(reinterpret_cast<char*>(s.buf.get() + done), static_cast<std::streamsize>(k)));
                if (ok) {
                    done += k;
                }
                std::scoped_lock lock(mutex);
                s.ready = done;
                if (!ok) {
                    s.failed = true;
                }
                if (stop) {
                    s.failed = s.ready < n;
                    ok = false;
                }
                cv.notify_all();
            }
            if (!ok) {
                std::scoped_lock lock(mutex);
                s.failed = s.ready < n;
                cv.notify_all();
            }
        }
    }

    Session::~Session() {
        {
            std::scoped_lock lock(mutex);
            stop = true;
        }
        if (reader.joinable()) {
            reader.join();
        }
    }

    bool Session::get(const std::string& key, const uint8_t*& data, size_t& size) {
        std::unique_lock<std::mutex> lock(mutex);
        auto it = by_key.find(key);
        if (it == by_key.end()) {
            return false;
        }
        Entry& e = entries[it->second];
        if (e.state != 1 && e.state != 2) {
            return false;
        }
        Span& s = *e.span;
        cv.wait(lock, [&] { return s.failed || s.ready >= e.span_offset + e.size; });
        if (s.ready < e.span_offset + e.size) {
            e.state = 3;
            dirty = true;
            return false;
        }
        const uint8_t* p = s.buf.get() + e.span_offset;
        if (e.state == 1) {
            lock.unlock();
            const bool good = hash64(p, static_cast<size_t>(e.size)) == e.hash;
            lock.lock();
            if (!good) {
                e.state = 3;
                dirty = true;
                return false;
            }
            e.state = 2;
        }
        data = p;
        size = static_cast<size_t>(e.size);
        return true;
    }

    std::vector<Session::Planned> Session::listed() {
        std::scoped_lock lock(mutex);
        std::vector<Planned> out;
        if (!pack_ok) {
            return out;
        }
        out.reserve(entries.size());
        for (const Entry& e : entries) {
            if (e.planned && e.size == 0 && e.state == 0) {
                continue; // planned by this session, not in the pack
            }
            out.push_back({e.key, e.stat});
        }
        return out;
    }

    void Session::reject(const std::string& key) {
        std::scoped_lock lock(mutex);
        auto it = by_key.find(key);
        if (it != by_key.end()) {
            entries[it->second].state = 3;
            dirty = true;
        }
    }

    bool Session::wants(const std::string& key) {
        std::scoped_lock lock(mutex);
        auto it = by_key.find(key);
        if (it == by_key.end()) {
            return false;
        }
        const Entry& e = entries[it->second];
        return e.planned && e.state != 2 && !e.fresh && fresh_bytes + e.stat.size <= kMaxPackBytes;
    }

    void Session::put(const std::string& key, std::vector<uint8_t> bytes) {
        std::scoped_lock lock(mutex);
        auto it = by_key.find(key);
        if (it == by_key.end()) {
            return;
        }
        Entry& e = entries[it->second];
        // a size other than the listing's: the file changed while the screen loaded
        if (!e.planned || e.fresh || bytes.size() != e.stat.size || fresh_bytes + bytes.size() > kMaxPackBytes) {
            return;
        }
        fresh_bytes += bytes.size();
        ++fresh_count;
        e.fresh = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
    }

    void Session::finish() {
        if (reader.joinable()) {
            reader.join(); // every span is read by now (or failed)
        }
        size_t used = 0;
        size_t planned = 0;
        for (const Entry& e : entries) {
            used += e.state == 2;
            planned += e.planned;
        }
        if (planned == 0) {
            return; // everything was still loaded (texture_retain)
        }
        spdlog::info("Texture pack for {}: {} of {} files from the pack, {} read from the skin, {} not read (BC7)",
                     screen, used, planned, fresh_count, bc7_skipped);
        if (fresh_count == 0) {
            return; // nothing new to store
        }
        Job job;
        job.path = path;
        job.screen = screen;
        job.old_payload_start = payload_start;
        uint64_t total = 0;
        for (Entry& e : entries) {
            WriteItem it;
            it.key = e.key;
            it.stat = e.stat;
            if (e.fresh) {
                it.data = e.fresh->data();
                it.owner = e.fresh;
            } else if (e.planned && (e.state == 1 || e.state == 2) && e.span &&
                       e.span->ready >= e.span_offset + e.size) {
                it.data = e.span->buf.get() + e.span_offset;
                it.owner = e.span;
                it.hash = e.hash;
                it.hashed = true;
            } else if ((!e.planned && e.carry && pack_ok) || (e.planned && e.state == 4)) {
                it.old_offset = e.offset;
                it.hash = e.hash;
                it.hashed = true;
            } else {
                continue;
            }
            if (total + it.stat.size > kMaxPackBytes) {
                continue;
            }
            total += it.stat.size;
            job.items.push_back(std::move(it));
        }
        writer().push(std::move(job));
    }

} // namespace texture_pack
