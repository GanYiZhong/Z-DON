#include "texture_lazy.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "global_data.h"
#include "raylib_ns.h"
#include "texture_retain.h"
#include "zdon_os.h"

// raylib.h (inside namespace ray) already defined RL_MATRIX_TYPE, so rlgl.h declares its functions with this
using ray::Matrix;
#include <rlgl.h>

namespace texture_lazy {
    namespace {

        constexpr size_t kMB = size_t{1} << 20;

        struct Pending {
            Image image{};
            int filter = -1;
            int wrap = -1;
            std::string screen;
            size_t bytes = 0;
            std::filesystem::path source;
            std::string owner;
            uint64_t file_size = 0, file_mtime = 0;
            bool evicted = false; // image freed at a screen change; source decodes it again
            // ahead-of-use upload in progress (warm_tick): the GL texture holding the first rows_done rows;
            // the Texture keeps id 0 until every row is in, so a draw before that finishes it at once
            unsigned int gl_id = 0;
            int rows_done = 0;
        };

        struct ScreenStats {
            size_t eager = 0, eager_bytes = 0;       // uploaded at load (off, gameplay, over the cap)
            size_t lazy = 0, lazy_bytes = 0;         // left pending at load
            size_t uploaded = 0, uploaded_bytes = 0; // pending ones uploaded since the last log
            double upload_ms = 0;
            size_t warmed = 0, warmed_bytes = 0;                   // uploaded ahead of their first use (warm_tick)
            size_t started_evicted = 0, started_evicted_bytes = 0; // make_evicted: never read at load
            double warm_ms = 0;
            bool cap_logged = false;
        };

        // never destroyed: textures may be freed by static destructors at exit
        std::unordered_map<const Texture*, Pending>& pending_map() {
            static auto* m = new std::unordered_map<const Texture*, Pending>();
            return *m;
        }
        std::map<std::string, ScreenStats>& stats() {
            static auto* m = new std::map<std::string, ScreenStats>();
            return *m;
        }
        size_t pending_bytes = 0;
        Image (*decoder)(const std::filesystem::path&) = nullptr;

        size_t cap_bytes() {
            static const size_t cap = [] {
                const char* v = std::getenv("ZDON_LAZY_UPLOAD_MB");
                const long mb = v && *v ? std::strtol(v, nullptr, 10) : zdon_os::mobile_cap_mb(768, 256);
                return mb > 0 ? static_cast<size_t>(mb) * kMB : size_t{0};
            }();
            return cap;
        }

        size_t image_bytes(int width, int height, int format) {
            return static_cast<size_t>(ray::GetPixelDataSize(width, height, format));
        }

        bool evict_enabled() {
            static const bool on = [] {
                const char* v = std::getenv("ZDON_LAZY_EVICT");
                return !(v && *v == '0');
            }();
            return on;
        }

        // Textures that may first be drawn in the middle of a song upload at load: there a first-use upload
        // is a dropped frame (measured: a 2352x3810 game texture 24 ms, a background stage 11 ms mid-song).
        // The global set is drawn during play too. Folders a Lua script loads while a song plays as well.
        bool during_gameplay(const std::string& screen) {
            if (screen == "global" || screen == "game" || screen == "background") {
                return true;
            }
            const std::string& now = global_data.current_screen;
            return now == "GAME" || now == "GAME_2P" || now == "GAME_DAN" || now == "GAME_PRACTICE" || now == "AI_GAME";
        }

        // drops a pending entry, freeing its image (and the GL texture of an unfinished ahead-of-use upload)
        void forget(std::unordered_map<const Texture*, Pending>::iterator it) {
            if (!it->second.evicted) {
                pending_bytes -= std::min(pending_bytes, it->second.bytes);
            }
            if (it->second.gl_id != 0) {
                ray::rlUnloadTexture(it->second.gl_id);
            }
            texture_retain::free_image_async(it->second.image);
            pending_map().erase(it);
        }

        // Warm-up (texture_lazy.h): the files whose pending textures were first drawn on each screen, in
        // the order they were first drawn, kept in cache/warm_upload.txt ("<owner>\t<source>" lines).
        constexpr size_t kWarmMaxPerOwner = 512;
        constexpr size_t kWarmBandBytes = size_t{1} << 20; // rows uploaded per glTexSubImage2D

        // Draws t once into a 1x1 target. The driver (NVIDIA) moves a texture into video memory on its first
        // draw, all at once (measured: a band-uploaded texture made the first frame that drew the difficulty
        // select 11-13 ms again, in the batch flush); a texture drawn while still empty is moved then, and the
        // bands uploaded after that go to it directly (the longest warm-up frame 15 -> 2.4 ms).
        void warm_touch(const Texture& t) {
            static RenderTarget rt = ray::LoadRenderTexture(1, 1); // never unloaded, like the GL context
            // back to whatever target was bound, with its matrices (a frame may be mid-draw)
            const ray::Matrix proj = ray::rlGetMatrixProjection();
            const ray::Matrix view = ray::rlGetMatrixModelview();
            begin_texture_mode(rt);
            ray::DrawTexturePro(t, Rect{0, 0, 1, 1}, Rect{0, 0, 1, 1}, Vec2{0, 0}, 0, ray::Color{255, 255, 255, 255});
            end_texture_mode();
            ray::rlSetMatrixProjection(proj);
            ray::rlSetMatrixModelview(view);
        }

        double warm_budget_ms() {
            static const double ms = [] {
                const char* v = std::getenv("ZDON_WARM_UPLOAD_MS");
                return v && *v ? std::max(0.0, std::atof(v)) : 1.0;
            }();
            return ms;
        }

        struct WarmRecords {
            bool loaded = false;
            bool dirty = false;
            std::map<std::string, std::vector<std::string>> by_owner;
            std::map<std::string, std::unordered_set<std::string>> known;
        };
        WarmRecords& warm_records() {
            static auto* r = new WarmRecords();
            if (!r->loaded) {
                r->loaded = true;
                std::ifstream in(std::filesystem::path("cache") / "warm_upload.txt");
                std::string line;
                while (std::getline(in, line)) {
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    const size_t tab = line.find('\t');
                    if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) {
                        continue;
                    }
                    std::string owner = line.substr(0, tab);
                    std::string source = line.substr(tab + 1);
                    auto& list = r->by_owner[owner];
                    if (list.size() < kWarmMaxPerOwner && r->known[owner].insert(source).second) {
                        list.push_back(std::move(source));
                    }
                }
            }
            return *r;
        }

        void warm_record(const std::string& owner, const std::filesystem::path& source) {
            if (warm_budget_ms() <= 0 || owner.empty() || source.empty()) {
                return;
            }
            WarmRecords& r = warm_records();
            const std::string key = source.generic_string();
            if (!r.known[owner].insert(key).second) {
                return;
            }
            auto& list = r.by_owner[owner];
            if (list.size() >= kWarmMaxPerOwner) {
                r.known[owner].erase(list.front());
                list.erase(list.begin());
            }
            list.push_back(key);
            r.dirty = true;
        }

        void warm_flush() {
            WarmRecords& r = warm_records();
            if (!r.dirty) {
                return;
            }
            r.dirty = false;
            const std::filesystem::path path = std::filesystem::path("cache") / "warm_upload.txt";
            std::filesystem::path tmp = path;
            tmp += ".tmp";
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                for (const auto& [owner, list] : r.by_owner) {
                    for (const std::string& source : list) {
                        out << owner << '\t' << source << '\n';
                    }
                }
                if (!out) {
                    spdlog::warn("Texture warm-up list not written: {}", tmp.string());
                    return;
                }
            }
            std::filesystem::rename(tmp, path, ec);
            if (ec) {
                spdlog::warn("Texture warm-up list not written: {} ({})", path.string(), ec.message());
                std::filesystem::remove(tmp, ec);
            }
        }

        // the textures warm_tick uploads, in order (checked against pending_map before each use)
        std::vector<const Texture*> warm_queue;
        size_t warm_pos = 0;
        size_t warm_ticks = 0; // frames warm_tick worked in since the last log, and its longest one
        double warm_tick_max_ms = 0;

        // the texture uploaded: hands it to t with the filter and wrap set while it was pending
        void finish_upload(const Texture* t, const Pending& p, const Texture& up) {
            auto* dst = const_cast<Texture*>(t); // heap object owned by make()'s shared_ptr
            *dst = up;
            if (p.wrap >= 0) {
                ray::SetTextureWrap(*dst, p.wrap);
            }
            if (p.filter >= 0) {
                ray::SetTextureFilter(*dst, p.filter);
            }
        }

        // A game texture that may wait for its first draw even though a song may be playing then: one that no
        // earlier load of this screen drew (cache/warm_upload.txt; those upload at load as before, never ahead
        // mid-song) and small enough to upload within a frame (ZDON_LAZY_GAME_MAX_KB, default 1024: a BC7
        // 1024x1024 or RGBA 512x512, a fraction of a millisecond). A song draws about 170 of Nijiiro's 984 game
        // textures; the rest used to take video memory and the driver's copy of it in RAM for nothing.
        // The same for the global set and the game backgrounds (also drawn while a song plays): textures no
        // earlier run drew (Nijiiro global: the AI battle and rainbow transitions...) wait for their first draw.
        // ZDON_LAZY_GAMEPLAY=0: every game, global and background texture uploads at load again.
        bool lazy_screen(const std::string& screen) {
            return screen == "game" || screen == "global" || screen == "background";
        }
        bool warm_known(const std::string& owner, const std::filesystem::path& source) {
            WarmRecords& r = warm_records();
            auto it = r.known.find(owner);
            return it != r.known.end() && it->second.contains(source.generic_string());
        }
        bool gameplay_lazy(const std::string& screen, const std::string& owner, const std::filesystem::path& source,
                           size_t bytes) {
            const size_t max_bytes = gameplay_lazy_max_bytes();
            if (max_bytes == 0 || !lazy_screen(screen) || bytes > max_bytes || owner.empty() || source.empty()) {
                return false;
            }
            WarmRecords& r = warm_records();
            auto it = r.known.find(owner);
            return it == r.known.end() || !it->second.contains(source.generic_string());
        }

        bool bandable(const Pending& p) {
            return p.image.mipmaps == 1 && p.image.format < ray::PIXELFORMAT_COMPRESSED_DXT1_RGB && p.image.height > 0;
        }

    } // namespace

    bool enabled() {
        static const bool on = [] {
            const bool def = zdon_os::texture_extras_by_default(); // Android/iOS: with enough memory
            const char* v = std::getenv("ZDON_LAZY_UPLOAD");
            return v && *v ? *v != '0' : def;
        }();
        return on;
    }

    std::shared_ptr<Texture> make(const Image& image, const std::string& screen, const std::filesystem::path& source,
                                  const std::string& owner, uint64_t size, uint64_t mtime) {
        if (!enabled() || !image.data) {
            return nullptr;
        }
        const size_t bytes = image_bytes(image.width, image.height, image.format);
        if (during_gameplay(screen) && !gameplay_lazy(screen, owner, source, bytes)) {
            return nullptr;
        }
        ScreenStats& st = stats()[screen];
        if (pending_bytes + bytes > cap_bytes()) {
            if (!st.cap_logged) {
                st.cap_logged = true;
                spdlog::info("Lazy texture upload: {} MB of images already wait in RAM (cap {} MB), the rest of {} "
                             "uploads at load",
                             pending_bytes / kMB, cap_bytes() / kMB, screen);
            }
            return nullptr;
        }
        auto* t = new Texture{};
        t->id = 0;
        t->width = image.width;
        t->height = image.height;
        t->mipmaps = image.mipmaps;
        t->format = image.format;
        pending_map().emplace(t, Pending{image, -1, -1, screen, bytes, source, owner, size, mtime, false});
        pending_bytes += bytes;
        ++st.lazy;
        st.lazy_bytes += bytes;
        return std::shared_ptr<Texture>(t, [](Texture* p) {
            auto& m = pending_map();
            if (auto it = m.find(p); it != m.end()) {
                forget(it);
            } else if (p->id != 0) {
                ray::UnloadTexture(*p);
            }
            delete p;
        });
    }

    bool starts_evicted(const std::string& screen, const std::string& owner, const std::filesystem::path& source,
                        int width, int height) {
        static const bool on = [] {
            const char* v = std::getenv("ZDON_LAZY_START_EVICTED");
            return !(v && *v == '0');
        }();
        if (!on || !enabled() || !evict_enabled() || !decoder || owner.empty() || source.empty()) {
            return false;
        }
        if (lazy_screen(screen)) {
            // the game / global / background textures that wait (gameplay_lazy) are let go too: a BC7 one is read
            // from the cache on its first draw, a fraction of a millisecond within ZDON_LAZY_GAME_MAX_KB
            return gameplay_lazy(screen, owner, source,
                                 image_bytes(width, height, ray::PIXELFORMAT_COMPRESSED_BPTC_RGBA));
        }
        return !during_gameplay(screen) && !warm_known(owner, source);
    }

    std::shared_ptr<Texture> make_evicted(int width, int height, int format, const std::string& screen,
                                          const std::filesystem::path& source, const std::string& owner, uint64_t size,
                                          uint64_t mtime) {
        auto* t = new Texture{};
        t->id = 0;
        t->width = width;
        t->height = height;
        t->mipmaps = 1;
        t->format = format;
        const size_t bytes = image_bytes(width, height, format);
        pending_map().emplace(t, Pending{Image{}, -1, -1, screen, bytes, source, owner, size, mtime, true});
        ScreenStats& st = stats()[screen];
        ++st.lazy;
        st.lazy_bytes += bytes;
        ++st.started_evicted;
        st.started_evicted_bytes += bytes;
        return std::shared_ptr<Texture>(t, [](Texture* p) {
            auto& m = pending_map();
            if (auto it = m.find(p); it != m.end()) {
                forget(it);
            } else if (p->id != 0) {
                ray::UnloadTexture(*p);
            }
            delete p;
        });
    }

    void count_eager(const std::string& screen, const Texture& t) {
        ScreenStats& st = stats()[screen];
        ++st.eager;
        st.eager_bytes += image_bytes(t.width, t.height, t.format);
    }

    void upload(const Texture* t) {
        if (!t || t->id != 0) {
            return;
        }
        auto& m = pending_map();
        auto it = m.find(t);
        if (it == m.end()) {
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        Pending& p = it->second;
        if (p.evicted) {
            // drawn before a screen load gave it its image back: decode it now (a hitch, hence the log)
            Image img = decoder ? decoder(p.source) : Image{};
            refill(t, img);
            spdlog::info("Lazy texture upload: {} decoded again on first use ({:.1f} ms)", p.source.string(),
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            if (p.evicted) {
                spdlog::error("Lazy texture upload: {} could not be decoded again", p.source.string());
                forget(it);
                return;
            }
        }
        Texture up{};
        if (p.gl_id != 0) {
            // warm_tick had uploaded the first rows: the rest now
            const size_t row_bytes = image_bytes(p.image.width, 1, p.image.format);
            ray::rlUpdateTexture(p.gl_id, 0, p.rows_done, p.image.width, p.image.height - p.rows_done, p.image.format,
                                 static_cast<const unsigned char*>(p.image.data) + row_bytes * p.rows_done);
            ray::rlDisableTexture();
            up.id = p.gl_id;
            up.width = p.image.width;
            up.height = p.image.height;
            up.mipmaps = p.image.mipmaps;
            up.format = p.image.format;
            p.gl_id = 0; // t owns it now
        } else {
            up = ray::LoadTextureFromImage(p.image);
        }
        if (!ray::IsTextureValid(up)) {
            spdlog::error("Lazy texture upload failed ({}x{}, {})", p.image.width, p.image.height, p.screen);
            forget(it);
            return;
        }
        finish_upload(t, p, up);
        ScreenStats& st = stats()[p.screen];
        ++st.uploaded;
        st.uploaded_bytes += p.bytes;
        st.upload_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        // ZDON_LAZY_UPLOAD_LOG=1 (dev): one line per texture uploaded on its first use
        static const bool log_each = std::getenv("ZDON_LAZY_UPLOAD_LOG") != nullptr;
        if (log_each) {
            spdlog::info("Lazy texture upload: {} ({}x{}, {}) uploaded on first use ({:.2f} ms)", p.source.string(),
                         t->width, t->height, p.owner,
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        warm_record(p.owner, p.source);
        forget(it);
    }

    void set_filter(const Texture* t, int filter) {
        if (!t) {
            return;
        }
        if (t->id == 0) {
            if (auto it = pending_map().find(t); it != pending_map().end()) {
                it->second.filter = filter;
            }
            return;
        }
        ray::SetTextureFilter(*const_cast<Texture*>(t), filter);
    }

    void set_wrap(const Texture* t, int wrap) {
        if (!t) {
            return;
        }
        if (t->id == 0) {
            if (auto it = pending_map().find(t); it != pending_map().end()) {
                it->second.wrap = wrap;
            }
            return;
        }
        ray::SetTextureWrap(*const_cast<Texture*>(t), wrap);
    }

    void evict_all() {
        warm_queue.clear();
        warm_pos = 0;
        warm_flush();
        if (!evict_enabled()) {
            return;
        }
        static const size_t keep_bytes = [] {
            const char* v = std::getenv("ZDON_LAZY_KEEP_MB");
            const long mb = v && *v ? std::strtol(v, nullptr, 10) : 0;
            return mb > 0 ? static_cast<size_t>(mb) * kMB : size_t{0};
        }();
        std::vector<Pending*> candidates;
        for (auto& [t, p] : pending_map()) {
            if (p.gl_id != 0) { // an ahead-of-use upload the screen did not finish: dropped, as if never begun
                ray::rlUnloadTexture(p.gl_id);
                p.gl_id = 0;
                p.rows_done = 0;
            }
            if (!p.evicted && !p.source.empty() && p.image.data) {
                candidates.push_back(&p);
            }
        }
        // ZDON_LAZY_KEEP_MB: the smallest images stay (per MB they cost the most to decode again)
        std::ranges::sort(candidates, {}, &Pending::bytes);
        size_t n = 0, bytes = 0, kept = 0;
        for (Pending* p : candidates) {
            if (kept + p->bytes <= keep_bytes) {
                kept += p->bytes;
                continue;
            }
            texture_retain::free_image_async(p->image);
            p->evicted = true;
            pending_bytes -= std::min(pending_bytes, p->bytes);
            ++n;
            bytes += p->bytes;
        }
        if (n > 0) {
            spdlog::info("Lazy texture upload: {} images of textures never drawn freed ({} MB, {} MB kept)", n,
                         bytes / kMB, kept / kMB);
        }
    }

    bool evicted(const Texture* t) {
        if (!t || t->id != 0) {
            return false;
        }
        auto it = pending_map().find(t);
        return it != pending_map().end() && it->second.evicted;
    }

    bool refill_wasted(const Texture* t) {
        if (!t || t->id != 0 || !decoder || !evict_enabled()) {
            return false;
        }
        auto it = pending_map().find(t);
        if (it == pending_map().end() || !it->second.evicted || it->second.source.empty()) {
            return false;
        }
        const Pending& p = it->second;
        // log_screen's test: such an image would be let go again right after the load
        return t->format >= ray::PIXELFORMAT_COMPRESSED_DXT1_RGB &&
               (lazy_screen(p.screen) || !warm_known(p.owner, p.source));
    }

    std::filesystem::path source(const Texture* t) {
        auto it = pending_map().find(t);
        return it != pending_map().end() ? it->second.source : std::filesystem::path{};
    }

    std::vector<std::filesystem::path> evicted_sources(const std::string& owner) {
        std::vector<std::filesystem::path> out;
        for (const auto& [t, p] : pending_map()) {
            if (p.evicted && p.owner == owner) {
                out.push_back(p.source);
            }
        }
        return out;
    }

    void refill(const Texture* t, Image image) {
        auto it = pending_map().find(t);
        if (it == pending_map().end() || !it->second.evicted || !image.data) {
            texture_retain::free_image_async(image);
            return;
        }
        Pending& p = it->second;
        // (the format may differ: Z-DON BC7 hands back blocks for an image made as RGBA8 before they were encoded)
        if (image.width != t->width || image.height != t->height) {
            // the file changed since the texture was made: its object was set up for the old size
            spdlog::warn("Lazy texture upload: {} changed on disk ({}x{} -> {}x{})", p.source.string(), t->width,
                         t->height, image.width, image.height);
        }
        p.image = image;
        p.bytes = image_bytes(image.width, image.height, image.format);
        p.evicted = false;
        pending_bytes += p.bytes;
    }

    std::vector<EvictedFile> evicted_files(const std::string& owner) {
        std::map<std::filesystem::path, EvictedFile> by_file;
        for (const auto& [t, p] : pending_map()) {
            if (p.evicted && p.owner == owner && !p.source.empty()) {
                by_file.try_emplace(p.source, EvictedFile{p.source, p.file_size, p.file_mtime,
                                                          image_bytes(t->width, t->height, t->format)});
            }
        }
        std::vector<EvictedFile> out;
        out.reserve(by_file.size());
        for (auto& [path, f] : by_file) {
            out.push_back(std::move(f));
        }
        return out;
    }

    size_t refill_from(const std::string& owner, std::vector<std::pair<std::filesystem::path, Image>>& images) {
        std::map<std::filesystem::path, std::vector<const Texture*>> by_file;
        for (const auto& [t, p] : pending_map()) {
            if (p.evicted && p.owner == owner) {
                by_file[p.source].push_back(t);
            }
        }
        size_t n = 0;
        for (auto& [path, image] : images) {
            auto it = by_file.find(path);
            if (it == by_file.end() || !image.data) {
                texture_retain::free_image_async(image);
                continue;
            }
            const auto& texs = it->second;
            for (size_t i = 0; i < texs.size(); ++i) {
                refill(texs[i], i + 1 < texs.size() ? image_copy(image) : image);
            }
            n += texs.size();
        }
        images.clear();
        return n;
    }

    size_t room_bytes() {
        return cap_bytes() > pending_bytes ? cap_bytes() - pending_bytes : 0;
    }

    unsigned prefill_jobs() {
        static const unsigned n = [] {
            const char* v = std::getenv("ZDON_LAZY_PREFILL");
            const long k = v && *v ? std::strtol(v, nullptr, 10) : 4;
            return k > 0 ? static_cast<unsigned>(std::min(k, 16L)) : 0U;
        }();
        return enabled() && evict_enabled() ? n : 0;
    }

    void set_decoder(Image (*decode)(const std::filesystem::path& source)) {
        decoder = decode;
    }

    void warm_tick() {
        if (warm_pos >= warm_queue.size()) {
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        const double budget = warm_budget_ms();
        auto elapsed = [&] {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        };
        auto& m = pending_map();
        while (warm_pos < warm_queue.size() && elapsed() < budget) {
            const Texture* t = warm_queue[warm_pos];
            auto it = m.find(t);
            if (it == m.end() || t->id != 0 || it->second.evicted || !it->second.image.data) {
                ++warm_pos; // drawn (uploaded) or gone since the queue was made
                continue;
            }
            Pending& p = it->second;
            const auto t0 = std::chrono::steady_clock::now();
            Texture up{};
            if (!bandable(p)) {
                up = ray::LoadTextureFromImage(p.image);
            } else {
                // a band of rows per call, so one big texture spreads over several frames
                if (p.gl_id == 0) {
                    p.gl_id = ray::rlLoadTexture(nullptr, p.image.width, p.image.height, p.image.format, 1);
                    if (p.gl_id == 0) {
                        ++warm_pos; // left for its first use
                        continue;
                    }
                    {
                        Texture empty{}; // into video memory while empty (warm_touch)
                        empty.id = p.gl_id;
                        empty.width = p.image.width;
                        empty.height = p.image.height;
                        empty.mipmaps = 1;
                        empty.format = p.image.format;
                        warm_touch(empty);
                    }
                }
                const size_t row_bytes = std::max<size_t>(1, image_bytes(p.image.width, 1, p.image.format));
                const int rows =
                    std::clamp(static_cast<int>(kWarmBandBytes / row_bytes), 1, p.image.height - p.rows_done);
                ray::rlUpdateTexture(p.gl_id, 0, p.rows_done, p.image.width, rows, p.image.format,
                                     static_cast<const unsigned char*>(p.image.data) + row_bytes * p.rows_done);
                ray::rlDisableTexture();
                p.rows_done += rows;
                ScreenStats& st = stats()[p.screen];
                st.warm_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                if (p.rows_done < p.image.height) {
                    continue;
                }
                up.id = p.gl_id;
                up.width = p.image.width;
                up.height = p.image.height;
                up.mipmaps = p.image.mipmaps;
                up.format = p.image.format;
                p.gl_id = 0;
            }
            ++warm_pos;
            if (!ray::IsTextureValid(up)) {
                continue; // left for its first use, which uploads it again (and logs a failure)
            }
            finish_upload(t, p, up);
            warm_touch(*t);
            ScreenStats& st = stats()[p.screen];
            ++st.warmed;
            st.warmed_bytes += p.bytes;
            if (!bandable(p)) {
                st.warm_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            }
            forget(it);
        }
        ++warm_ticks;
        warm_tick_max_ms = std::max(warm_tick_max_ms, elapsed());
        static const bool log_each = std::getenv("ZDON_LAZY_UPLOAD_LOG") != nullptr;
        if (log_each) {
            spdlog::info("Lazy texture upload: warm tick {:.2f} ms", elapsed());
        }
    }

    bool bandable(const Image& image) {
        return image.data && image.mipmaps == 1 && image.format < ray::PIXELFORMAT_COMPRESSED_DXT1_RGB &&
               image.height > 0;
    }

    bool upload_band(const Image& image, BandUpload& st) {
        if (!bandable(image)) {
            return false;
        }
        if (st.id == 0) {
            st.id = ray::rlLoadTexture(nullptr, image.width, image.height, image.format, 1);
            if (st.id == 0) {
                return false;
            }
            Texture empty{}; // into video memory while empty (warm_touch)
            empty.id = st.id;
            empty.width = image.width;
            empty.height = image.height;
            empty.mipmaps = 1;
            empty.format = image.format;
            warm_touch(empty);
            st.rows_done = 0;
        }
        const size_t row_bytes = std::max<size_t>(1, image_bytes(image.width, 1, image.format));
        const int rows = std::clamp(static_cast<int>(kWarmBandBytes / row_bytes), 1, image.height - st.rows_done);
        ray::rlUpdateTexture(st.id, 0, st.rows_done, image.width, rows, image.format,
                             static_cast<const unsigned char*>(image.data) + row_bytes * st.rows_done);
        ray::rlDisableTexture();
        st.rows_done += rows;
        if (st.rows_done < image.height) {
            return false;
        }
        Texture done{};
        done.id = st.id;
        done.width = image.width;
        done.height = image.height;
        done.mipmaps = 1;
        done.format = image.format;
        warm_touch(done);
        return true;
    }

    size_t gameplay_lazy_max_bytes() {
        static const size_t max_bytes = [] {
            const char* off = std::getenv("ZDON_LAZY_GAMEPLAY");
            if (off && *off == '0') {
                return size_t{0};
            }
            const char* v = std::getenv("ZDON_LAZY_GAME_MAX_KB");
            return static_cast<size_t>(v && *v ? std::max(0L, std::atol(v)) : 1024L) * 1024;
        }();
        return max_bytes;
    }

    std::unordered_set<std::string> warm_sources(const std::string& owner) {
        WarmRecords& r = warm_records();
        auto it = r.known.find(owner);
        return it == r.known.end() ? std::unordered_set<std::string>{} : it->second;
    }

    void log_screen(const std::string& screen_name) {
        // Game, global and background textures left for their first draw (gameplay_lazy) whose image is BC7, and
        // those of any other screen that no earlier run drew (not in the warm-up list; Nijiiro song select: about
        // 1230 of 1250, 190 MB): not kept in RAM either. The first draw reads them back from the BC7 cache (no PNG
        // decode: cached_image), a fraction of a millisecond for <= 1 MB; RGBA ones stay (decoding the PNG again would
        // cost more).
        if (decoder && evict_enabled()) {
            size_t n = 0, bytes = 0;
            for (auto& [t, p] : pending_map()) {
                if ((lazy_screen(p.screen) || !warm_known(p.owner, p.source)) && !p.evicted && p.gl_id == 0 &&
                    !p.source.empty() && p.image.data && p.image.format >= ray::PIXELFORMAT_COMPRESSED_DXT1_RGB) {
                    texture_retain::free_image_async(p.image);
                    p.image = Image{};
                    p.evicted = true;
                    pending_bytes -= std::min(pending_bytes, p.bytes);
                    ++n;
                    bytes += p.bytes;
                }
            }
            if (n > 0) {
                spdlog::info("Lazy texture upload: {} compressed images of textures waiting for a first draw not kept "
                             "in RAM ({} "
                             "MB); read back from the BC7 cache on first draw",
                             n, bytes / kMB);
            }
        }
        for (auto& [screen, st] : stats()) {
            if (st.uploaded > 0) {
                spdlog::info("Lazy texture upload: {} pending textures of {} uploaded on first use since the last "
                             "screen load ({} MB, {:.1f} ms)",
                             st.uploaded, screen, st.uploaded_bytes / kMB, st.upload_ms);
                st.uploaded = 0;
                st.uploaded_bytes = 0;
                st.upload_ms = 0;
            }
            if (st.warmed > 0) {
                spdlog::info("Lazy texture upload: {} pending textures of {} uploaded ahead of their first use since "
                             "the last screen load ({} MB, {:.1f} ms)",
                             st.warmed, screen, st.warmed_bytes / kMB, st.warm_ms);
                st.warmed = 0;
                st.warmed_bytes = 0;
                st.warm_ms = 0;
            }
        }
        if (warm_ticks > 0) {
            spdlog::info("Lazy texture upload: ahead-of-use uploads ran in {} frames, longest {:.2f} ms", warm_ticks,
                         warm_tick_max_ms);
            warm_ticks = 0;
            warm_tick_max_ms = 0;
        }
        // what warm_tick uploads while this screen shows: its pending textures first drawn the last times
        warm_queue.clear();
        warm_pos = 0;
        if (warm_budget_ms() > 0) {
            WarmRecords& r = warm_records();
            if (auto lit = r.by_owner.find(screen_name); lit != r.by_owner.end()) {
                std::unordered_map<std::string, std::vector<const Texture*>> by_source;
                for (const auto& [t, p] : pending_map()) {
                    if (p.owner == screen_name && !p.evicted && p.image.data) {
                        by_source[p.source.generic_string()].push_back(t);
                    }
                }
                for (const std::string& source : lit->second) {
                    if (auto sit = by_source.find(source); sit != by_source.end()) {
                        warm_queue.insert(warm_queue.end(), sit->second.begin(), sit->second.end());
                    }
                }
            }
            if (!warm_queue.empty()) {
                spdlog::info("Lazy texture upload for {}: {} textures drawn the last times to upload ahead, {:.1f} ms "
                             "per frame",
                             screen_name, warm_queue.size(), warm_budget_ms());
            }
        }
        auto it = stats().find(screen_name);
        if (it == stats().end()) {
            return;
        }
        ScreenStats& st = it->second;
        spdlog::info("Lazy texture upload for {}: {} uploaded at load ({} MB), {} pending ({} MB); all screens: {} "
                     "pending, {} MB in RAM (cap {} MB)",
                     screen_name, st.eager, st.eager_bytes / kMB, st.lazy, st.lazy_bytes / kMB, pending_map().size(),
                     pending_bytes / kMB, cap_bytes() / kMB);
        if (st.started_evicted > 0) {
            spdlog::info("Lazy texture upload for {}: {} of the pending BC7 images not read at load ({} MB); read from "
                         "the BC7 cache on first draw",
                         screen_name, st.started_evicted, st.started_evicted_bytes / kMB);
        }
        st.eager = st.eager_bytes = st.lazy = st.lazy_bytes = 0;
        st.started_evicted = st.started_evicted_bytes = 0;
        st.cap_logged = false;
    }

} // namespace texture_lazy
