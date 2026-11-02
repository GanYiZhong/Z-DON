#include "texture_bc7.h"

#include <algorithm>
#include <atomic>
#include <bc7decomp.h>
#include <bc7enc.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef ZDON_HAVE_ZLIB
#include <zlib.h>
#else
#include <miniz.h>
#endif

#include "jobs.h"
#include "raylib_ns.h"

// raylib.h (inside namespace ray) already defined RL_MATRIX_TYPE, so rlgl.h declares its functions with this
using ray::Matrix;
#include <rlgl.h>

namespace fs = std::filesystem;

namespace texture_bc7 {
    namespace {
        constexpr int kMaxError = 4;                       // per channel, of 255, on visible pixels
        constexpr double kMaxMse = 2.0592;                 // or PSNR >= 45 dB over the image: 255^2 / 10^4.5
        constexpr size_t kQueueCap = size_t{256} << 20;    // pixels waiting for an encode job
        constexpr uint64_t kFileCap = uint64_t{256} << 20; // cache file
        constexpr size_t kBorderCap = 16384;               // remembered borders
        constexpr char kMagic[8] = {'Z', 'D', 'B', 'C', '7', 0, 0, 1};
        constexpr uint32_t kVersion = 2; // encoder settings / kMaxError / kMaxMse: a change starts over
        constexpr size_t kRecordHeader = 8 + 4 + 4 + 1 + 4;
        constexpr int kFormat = ray::PIXELFORMAT_COMPRESSED_BPTC_RGBA;
        constexpr double kMB = 1024.0 * 1024.0;

        enum class Kind : uint8_t {
            Blocks, // deflated BC7 blocks
            Keep,   // over kMaxError and kMaxMse: stays RGBA8
        };
        struct Entry {
            Kind kind = Kind::Keep;
            uint32_t width = 0, height = 0;
            uint64_t offset = 0; // of the deflated blocks in the cache file, when mem is empty
            uint32_t size = 0;
            std::shared_ptr<const std::vector<uint8_t>> mem; // encoded this run, not in the file yet
            bool used = false;                               // this run: kept when the file is over kFileCap
        };

        struct State {
            std::atomic<bool> on{false};
            std::shared_future<void> loaded;
            std::shared_mutex mutex; // entries, queue, flags
            std::unordered_map<uint64_t, Entry> entries;
            std::unordered_set<uint64_t> queued;
            size_t queued_bytes = 0;
            size_t dirty = 0;
            bool saving = false;
            bool stopping = false;
            // the cache file, read on demand (the blocks are not kept in memory); taken before mutex
            std::mutex file_mutex;
            FILE* reader = nullptr;
            // stats
            std::atomic<size_t> hits{0}, kept{0}, encoded{0}, rejected{0};
            std::atomic<uint64_t> raw_bytes{0}, bc7_bytes{0};

            std::mutex border_mutex;
            std::unordered_map<uint64_t, Rect> borders; // hash of BC7 blocks -> border before compression
        };
        State& state() {
            static auto* s = new State(); // never destroyed: encode jobs may outlive static teardown
            return *s;
        }

        fs::path cache_path() {
            return fs::path("cache") / "bc7" / "textures.bin";
        }

        FILE* open_file(const fs::path& p, const char* mode) {
#ifdef _WIN32
            return _wfopen(p.wstring().c_str(), mode[0] == 'r' ? L"rb" : L"wb");
#else
            return std::fopen(p.string().c_str(), mode);
#endif
        }

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

        size_t block_bytes(uint32_t w, uint32_t h) {
            return size_t{(w + 3) / 4} * ((h + 3) / 4) * 16;
        }

        bool encodable(const Image& img) {
            return img.data && img.mipmaps == 1 && img.width > 0 && img.height > 0 &&
                   (img.format == ray::PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 ||
                    img.format == ray::PIXELFORMAT_UNCOMPRESSED_R8G8B8);
        }

        // the image's pixels as RGBA8 (a copy for RGB8)
        std::vector<uint8_t> rgba_copy(const Image& img) {
            const size_t n = size_t(img.width) * img.height;
            std::vector<uint8_t> out(n * 4);
            const auto* src = static_cast<const uint8_t*>(img.data);
            if (img.format == ray::PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) {
                std::memcpy(out.data(), src, n * 4);
            } else {
                for (size_t i = 0; i < n; ++i) {
                    out[i * 4] = src[i * 3];
                    out[i * 4 + 1] = src[i * 3 + 1];
                    out[i * 4 + 2] = src[i * 3 + 2];
                    out[i * 4 + 3] = 255;
                }
            }
            return out;
        }

        uint64_t key_of(const Image& img) {
            const size_t bpp = img.format == ray::PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 ? 4 : 3;
            const uint64_t h = hash64(static_cast<const uint8_t*>(img.data), size_t(img.width) * img.height * bpp);
            return h ^ ((uint64_t(uint32_t(img.width)) << 32 | uint32_t(img.height)) * 0x9E3779B97F4A7C15ull) ^ bpp;
        }

        // GetImageAlphaBorder(img, 0)
        Rect border_of(const uint8_t* rgba, int w, int h) {
            int x0 = w, x1 = -1, y0 = h, y1 = -1;
            for (int y = 0; y < h; ++y) {
                const uint8_t* row = rgba + size_t(y) * w * 4;
                int first = -1, last = -1;
                for (int x = 0; x < w; ++x) {
                    if (row[x * 4 + 3] > 0) {
                        first = x;
                        break;
                    }
                }
                if (first < 0) {
                    continue;
                }
                for (int x = w - 1; x >= first; --x) {
                    if (row[x * 4 + 3] > 0) {
                        last = x;
                        break;
                    }
                }
                x0 = std::min(x0, first);
                x1 = std::max(x1, last);
                y0 = std::min(y0, y);
                y1 = y;
            }
            if (x1 < 0) {
                return Rect{};
            }
            return Rect{float(x0), float(y0), float(x1 + 1 - x0), float(y1 + 1 - y0)};
        }

        bool inflate_to(const uint8_t* z, size_t zn, uint8_t* out, size_t n) {
#ifdef ZDON_HAVE_ZLIB
            uLongf got = static_cast<uLongf>(n);
            return uncompress(out, &got, z, static_cast<uLong>(zn)) == Z_OK && got == n;
#else
            mz_ulong got = static_cast<mz_ulong>(n);
            return mz_uncompress(out, &got, z, static_cast<mz_ulong>(zn)) == MZ_OK && got == n;
#endif
        }

        std::vector<uint8_t> deflate_blocks(const std::vector<uint8_t>& in) {
#ifdef ZDON_HAVE_ZLIB
            uLongf n = compressBound(static_cast<uLong>(in.size()));
            std::vector<uint8_t> out(n);
            if (compress2(out.data(), &n, in.data(), static_cast<uLong>(in.size()), 6) != Z_OK) {
                return {};
            }
#else
            mz_ulong n = mz_compressBound(static_cast<mz_ulong>(in.size()));
            std::vector<uint8_t> out(n);
            if (mz_compress2(out.data(), &n, in.data(), static_cast<mz_ulong>(in.size()), 6) != MZ_OK) {
                return {};
            }
#endif
            out.resize(n);
            return out;
        }

        template<class T>
        void put(FILE* f, T v) {
            std::fwrite(&v, sizeof(T), 1, f);
        }
        template<class T>
        bool get(FILE* f, T& v) {
            return std::fread(&v, sizeof(T), 1, f) == 1;
        }

        // e's deflated blocks; file_mutex held when e is in the file
        bool read_payload_locked(const Entry& e, std::vector<uint8_t>& out) {
            State& s = state();
            if (e.mem) {
                out = *e.mem;
                return true;
            }
            out.resize(e.size);
            return s.reader && std::fseek(s.reader, static_cast<long>(e.offset), SEEK_SET) == 0 &&
                   std::fread(out.data(), 1, e.size, s.reader) == e.size;
        }

        // the index of the cache file (record: u64 key, u32 width, u32 height, u8 kind, u32 size, size bytes); the
        // file stays open for the payloads
        void load_file() {
            State& s = state();
            std::scoped_lock file_lock(s.file_mutex);
            FILE* f = open_file(cache_path(), "r");
            if (!f) {
                return;
            }
            char magic[sizeof(kMagic)] = {};
            uint32_t version = 0;
            if (std::fread(magic, 1, sizeof(magic), f) != sizeof(magic) ||
                std::memcmp(magic, kMagic, sizeof(kMagic)) != 0 || !get(f, version) || version != kVersion) {
                spdlog::info("BC7 texture cache: {} is from another version, starting over", cache_path().string());
                std::fclose(f);
                return;
            }
            std::fseek(f, 0, SEEK_END);
            const long file_size = std::ftell(f);
            long pos = static_cast<long>(sizeof(kMagic) + sizeof(version));
            std::unique_lock lock(s.mutex);
            while (pos < file_size) {
                uint64_t key = 0;
                uint32_t w = 0, h = 0, size = 0;
                uint8_t kind = 0;
                if (std::fseek(f, pos, SEEK_SET) != 0 || !get(f, key) || !get(f, w) || !get(f, h) || !get(f, kind) ||
                    !get(f, size) || pos + static_cast<long>(kRecordHeader + size) > file_size ||
                    kind > uint8_t(Kind::Keep)) {
                    spdlog::warn("BC7 texture cache: {} is cut short, using {} entries", cache_path().string(),
                                 s.entries.size());
                    break;
                }
                Entry e;
                e.kind = Kind(kind);
                e.width = w;
                e.height = h;
                e.offset = static_cast<uint64_t>(pos) + kRecordHeader;
                e.size = size;
                s.entries[key] = std::move(e);
                pos += static_cast<long>(kRecordHeader + size);
            }
            s.reader = f;
            spdlog::info("BC7 texture cache: {} entries ({:.1f} MB)", s.entries.size(), file_size / kMB);
        }

        // the cache file from the entries (a Background job, or shutdown); afterwards the entries point into it
        void write_file() {
            State& s = state();
            std::scoped_lock file_lock(s.file_mutex);
            std::vector<std::pair<uint64_t, Entry>> list;
            {
                std::unique_lock lock(s.mutex);
                s.dirty = 0;
                uint64_t total = 0;
                for (const auto& [key, e] : s.entries) {
                    total += kRecordHeader + e.size;
                }
                const bool trim = total > kFileCap; // keep only what this run used
                list.reserve(s.entries.size());
                for (const auto& [key, e] : s.entries) {
                    if (!trim || e.used) {
                        list.emplace_back(key, e);
                    }
                }
            }
            std::error_code ec;
            fs::create_directories(cache_path().parent_path(), ec);
            fs::path tmp = cache_path();
            tmp += ".tmp";
            FILE* f = open_file(tmp, "w");
            if (!f) {
                spdlog::warn("BC7 texture cache: could not write {}", tmp.string());
                return;
            }
            std::fwrite(kMagic, 1, sizeof(kMagic), f);
            put(f, kVersion);
            uint64_t pos = sizeof(kMagic) + sizeof(kVersion);
            std::vector<uint64_t> offsets(list.size(), 0); // 0: not written
            std::vector<uint8_t> payload;
            for (size_t i = 0; i < list.size(); ++i) {
                const auto& [key, e] = list[i];
                if (!read_payload_locked(e, payload)) {
                    continue;
                }
                put(f, key);
                put(f, e.width);
                put(f, e.height);
                put(f, uint8_t(e.kind));
                put(f, e.size);
                std::fwrite(payload.data(), 1, payload.size(), f);
                offsets[i] = pos + kRecordHeader;
                pos += kRecordHeader + payload.size();
            }
            const bool ok = std::ferror(f) == 0;
            std::fclose(f);
            if (!ok) {
                spdlog::warn("BC7 texture cache: could not write {}", tmp.string());
                return;
            }
            // Windows cannot replace a file that is open: close the reader for the rename
            if (s.reader) {
                std::fclose(s.reader);
                s.reader = nullptr;
            }
            fs::rename(tmp, cache_path(), ec);
            s.reader = open_file(cache_path(), "r");
            if (ec) {
                spdlog::warn("BC7 texture cache: could not replace {}: {}", cache_path().string(), ec.message());
                return; // the old file (and its offsets) stay
            }
            size_t written = 0;
            {
                std::unique_lock lock(s.mutex);
                std::unordered_set<uint64_t> in_file;
                for (size_t i = 0; i < list.size(); ++i) {
                    if (offsets[i] == 0) {
                        continue;
                    }
                    ++written;
                    in_file.insert(list[i].first);
                    auto it = s.entries.find(list[i].first);
                    if (it != s.entries.end()) {
                        it->second.offset = offsets[i];
                        it->second.mem.reset();
                    }
                }
                // what the file left out is gone, unless it is new since the list was made
                std::erase_if(s.entries, [&](const auto& kv) { return !kv.second.mem && !in_file.contains(kv.first); });
            }
            spdlog::info("BC7 texture cache: saved {} entries ({:.1f} MB)", written, pos / kMB);
        }

        // write_file, again while encodes finished meanwhile
        void save_job() {
            State& s = state();
            while (true) {
                {
                    std::unique_lock lock(s.mutex);
                    if (s.stopping) { // shutdown writes the file itself
                        s.saving = false;
                        return;
                    }
                }
                write_file();
                std::unique_lock lock(s.mutex);
                if (s.dirty == 0 || !s.queued.empty() || s.stopping) {
                    s.saving = false;
                    return;
                }
            }
        }

        // max per-channel error of a block on its visible pixels (colour weighted by alpha, as it is drawn);
        // adds the squared errors to sse
        int block_error(const uint8_t* block, const uint8_t* px, uint64_t& sse) {
            bc7decomp::color_rgba out[16];
            if (!bc7decomp::unpack_bc7(block, out)) {
                sse += uint64_t{255 * 255} * 64;
                return 255;
            }
            int worst = 0;
            for (int i = 0; i < 16; ++i) {
                const uint8_t* s = px + i * 4;
                const int a = std::max<int>(s[3], out[i].m_comps[3]);
                const int ea = std::abs(int(s[3]) - int(out[i].m_comps[3]));
                worst = std::max(worst, ea);
                sse += uint64_t(ea * ea);
                for (int c = 0; c < 3; ++c) {
                    const int e = std::abs(int(s[c]) - int(out[i].m_comps[c])) * a / 255;
                    worst = std::max(worst, e);
                    sse += uint64_t(e * e);
                }
            }
            return worst;
        }

        void encode_job(uint64_t key, uint32_t w, uint32_t h, std::vector<uint8_t> rgba) {
            State& s = state();
            static std::once_flag init_once;
            std::call_once(init_once, [] { bc7enc_compress_block_init(); });
            {
                std::shared_lock lock(s.mutex);
                if (s.stopping) {
                    return;
                }
            }
            bc7enc_compress_block_params params;
            bc7enc_compress_block_params_init(&params);
            bc7enc_compress_block_params_init_linear_weights(&params);
            params.m_uber_level = 0;

            const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
            std::vector<uint8_t> blocks(block_bytes(w, h));
            uint64_t sse = 0;
            int worst = 0;
            for (uint32_t by = 0; by < bh; ++by) {
                for (uint32_t bx = 0; bx < bw; ++bx) {
                    uint8_t px[64];
                    for (uint32_t y = 0; y < 4; ++y) {
                        for (uint32_t x = 0; x < 4; ++x) {
                            const uint32_t sx = std::min(bx * 4 + x, w - 1), sy = std::min(by * 4 + y, h - 1);
                            std::memcpy(px + (y * 4 + x) * 4, &rgba[(size_t(sy) * w + sx) * 4], 4);
                        }
                    }
                    uint8_t* block = &blocks[(size_t(by) * bw + bx) * 16];
                    bc7enc_compress_block(block, px, &params);
                    worst = std::max(worst, block_error(block, px, sse));
                }
            }
            // painted skins (gradients) rarely stay under kMaxError everywhere; a small error over the
            // whole image is accepted too
            const double mse = double(sse) / (double(bw) * bh * 64);
            const bool ok = worst <= kMaxError || mse <= kMaxMse;
            std::vector<uint8_t> z = ok ? deflate_blocks(blocks) : std::vector<uint8_t>{};
            const bool keep = z.empty();

            std::unique_lock lock(s.mutex);
            s.queued.erase(key);
            s.queued_bytes -= std::min(s.queued_bytes, rgba.size());
            if (s.stopping) {
                return;
            }
            Entry e;
            e.kind = keep ? Kind::Keep : Kind::Blocks;
            e.width = w;
            e.height = h;
            e.used = true;
            e.size = static_cast<uint32_t>(z.size());
            e.mem = std::make_shared<const std::vector<uint8_t>>(std::move(z));
            s.entries[key] = std::move(e);
            ++s.dirty;
            (keep ? s.rejected : s.encoded)++;
            if (s.queued.empty() && !s.saving) {
                s.saving = true;
                lock.unlock();
                jobs::submit(jobs::Priority::Background, save_job);
            }
        }

        void remember_border(const uint8_t* blocks, size_t n, Rect border) {
            State& s = state();
            const uint64_t h = hash64(blocks, n);
            std::scoped_lock lock(s.border_mutex);
            if (s.borders.size() >= kBorderCap) {
                s.borders.clear(); // a border is asked for right after its image is loaded
            }
            s.borders[h] = border;
        }
    } // namespace

    void init(bool configured) {
        State& s = state();
        bool want = configured;
        if (const char* v = std::getenv("ZDON_BC7")) {
            want = v[0] == '1';
        }
        const bool supported = ray::rlIsTextureCompressionBPTCSupported();
        const bool on = want && supported;
        if (want && !supported) {
            spdlog::info("BC7 textures: off (the GL driver has no BPTC)");
        } else {
            spdlog::info("BC7 textures: {}", on ? "on" : "off");
        }
        if (on) {
            s.loaded = std::async(std::launch::async, load_file).share();
        }
        s.on.store(on, std::memory_order_release); // after loaded: compress waits on it
    }

    bool enabled() {
        return state().on.load(std::memory_order_acquire);
    }

    void compress(Image& image) {
        State& s = state();
        if (!enabled() || !encodable(image)) {
            return;
        }
        s.loaded.wait();
        const uint64_t key = key_of(image);
        const uint32_t w = static_cast<uint32_t>(image.width), h = static_cast<uint32_t>(image.height);
        Entry e;
        bool found = false;
        {
            std::shared_lock lock(s.mutex);
            auto it = s.entries.find(key);
            if (it != s.entries.end() && it->second.width == w && it->second.height == h) {
                e = it->second;
                found = true;
            }
        }
        if (found) {
            if (!e.used) {
                std::unique_lock lock(s.mutex);
                if (auto it = s.entries.find(key); it != s.entries.end()) {
                    it->second.used = true;
                }
            }
            if (e.kind == Kind::Keep) {
                ++s.kept;
                return;
            }
            std::vector<uint8_t> z;
            bool read = false;
            if (e.mem) {
                read = read_payload_locked(e, z);
            } else {
                std::scoped_lock file_lock(s.file_mutex);
                // the file may have been rewritten since e was copied: take the entry's offset again
                std::shared_lock lock(s.mutex);
                if (auto it = s.entries.find(key); it != s.entries.end()) {
                    read = read_payload_locked(it->second, z);
                }
            }
            const size_t n = block_bytes(w, h);
            auto* blocks = read ? static_cast<uint8_t*>(std::malloc(n)) : nullptr; // RL_MALLOC: UnloadImage frees it
            if (!blocks || !inflate_to(z.data(), z.size(), blocks, n)) {
                std::free(blocks);
                spdlog::warn("BC7 texture cache: an entry ({}x{}) could not be read", w, h);
                return;
            }
            const bool rgba = image.format == ray::PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            const Rect border = rgba ? border_of(static_cast<const uint8_t*>(image.data), image.width, image.height)
                                     : Rect{0, 0, float(w), float(h)};
            remember_border(blocks, n, border);
            s.raw_bytes += size_t(w) * h * (rgba ? 4 : 3);
            s.bc7_bytes += n;
            ++s.hits;
            ray::UnloadImage(image);
            image.data = blocks;
            image.format = kFormat;
            return;
        }
        // not known yet: encode for the next load
        const size_t bytes = size_t(w) * h * 4;
        {
            std::unique_lock lock(s.mutex);
            if (s.stopping || s.queued.contains(key) || s.queued_bytes + bytes > kQueueCap) {
                return;
            }
            s.queued.insert(key);
            s.queued_bytes += bytes;
        }
        jobs::submit(jobs::Priority::Background,
                     [key, w, h, px = rgba_copy(image)]() mutable { encode_job(key, w, h, std::move(px)); });
    }

    Rect alpha_border(const Image& image) {
        State& s = state();
        const int w = image.width, h = image.height;
        const auto* blocks = static_cast<const uint8_t*>(image.data);
        const size_t n = block_bytes(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        {
            const uint64_t key = hash64(blocks, n);
            std::scoped_lock lock(s.border_mutex);
            auto it = s.borders.find(key);
            if (it != s.borders.end()) {
                return it->second;
            }
        }
        // not remembered: the border of the decoded blocks
        std::vector<uint8_t> rgba(size_t(w) * h * 4);
        const int bw = (w + 3) / 4, bh = (h + 3) / 4;
        for (int by = 0; by < bh; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                bc7decomp::color_rgba out[16];
                bc7decomp::unpack_bc7(blocks + (size_t(by) * bw + bx) * 16, out);
                for (int y = 0; y < 4 && by * 4 + y < h; ++y) {
                    for (int x = 0; x < 4 && bx * 4 + x < w; ++x) {
                        std::memcpy(&rgba[(size_t(by * 4 + y) * w + bx * 4 + x) * 4], out[y * 4 + x].m_comps, 4);
                    }
                }
            }
        }
        return border_of(rgba.data(), w, h);
    }

    void shutdown() {
        State& s = state();
        if (!enabled()) {
            return;
        }
        spdlog::info("BC7 textures: {} images from the cache ({:.0f} MB as RGBA, {:.0f} MB as BC7), {} kept as RGBA, "
                     "{} encoded this run, {} over the error limit",
                     s.hits.load(), s.raw_bytes.load() / kMB, s.bc7_bytes.load() / kMB, s.kept.load(), s.encoded.load(),
                     s.rejected.load());
        bool save = false;
        {
            std::unique_lock lock(s.mutex);
            s.stopping = true; // queued encodes and saves return
            save = s.dirty > 0;
        }
        if (save) {
            write_file(); // waits for a save that is running
        }
    }
} // namespace texture_bc7
