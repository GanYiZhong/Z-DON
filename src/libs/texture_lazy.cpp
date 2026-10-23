#include "texture_lazy.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <vector>

#include "global_data.h"
#include "texture_retain.h"

namespace texture_lazy {
    namespace {

        constexpr size_t kMB = size_t{1} << 20;

        struct Pending {
            ray::Image image{};
            int filter = -1;
            int wrap = -1;
            std::string screen;
            size_t bytes = 0;
            std::filesystem::path source;
            std::string owner;
            uint64_t file_size = 0, file_mtime = 0;
            bool evicted = false; // image freed at a screen change; source decodes it again
        };

        struct ScreenStats {
            size_t eager = 0, eager_bytes = 0;       // uploaded at load (off, gameplay, over the cap)
            size_t lazy = 0, lazy_bytes = 0;         // left pending at load
            size_t uploaded = 0, uploaded_bytes = 0; // pending ones uploaded since the last log
            double upload_ms = 0;
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
        ray::Image (*decoder)(const std::filesystem::path&) = nullptr;

        size_t cap_bytes() {
            static const size_t cap = [] {
                const char* v = std::getenv("ZDON_LAZY_UPLOAD_MB");
                const long mb = v && *v ? std::strtol(v, nullptr, 10) : 768;
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

        // drops a pending entry, freeing its image
        void forget(std::unordered_map<const Texture*, Pending>::iterator it) {
            if (!it->second.evicted) {
                pending_bytes -= std::min(pending_bytes, it->second.bytes);
            }
            texture_retain::free_image_async(it->second.image);
            pending_map().erase(it);
        }

    } // namespace

    bool enabled() {
        static const bool on = [] {
#if defined(PLATFORM_ANDROID) || defined(YATAIDON_PLATFORM_IOS) || defined(__EMSCRIPTEN__)
            const bool def = false;
#else
            const bool def = true;
#endif
            const char* v = std::getenv("ZDON_LAZY_UPLOAD");
            return v && *v ? *v != '0' : def;
        }();
        return on;
    }

    std::shared_ptr<Texture> make(const ray::Image& image, const std::string& screen,
                                         const std::filesystem::path& source, const std::string& owner, uint64_t size,
                                         uint64_t mtime) {
        if (!enabled() || !image.data || during_gameplay(screen)) {
            return nullptr;
        }
        const size_t bytes = image_bytes(image.width, image.height, image.format);
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
            ray::Image img = decoder ? decoder(p.source) : ray::Image{};
            refill(t, img);
            spdlog::info("Lazy texture upload: {} decoded again on first use ({:.1f} ms)", p.source.string(),
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            if (p.evicted) {
                spdlog::error("Lazy texture upload: {} could not be decoded again", p.source.string());
                forget(it);
                return;
            }
        }
        Texture up = ray::LoadTextureFromImage(p.image);
        if (!ray::IsTextureValid(up)) {
            spdlog::error("Lazy texture upload failed ({}x{}, {})", p.image.width, p.image.height, p.screen);
            forget(it);
            return;
        }
        auto* dst = const_cast<Texture*>(t); // heap object owned by make()'s shared_ptr
        *dst = up;
        if (p.wrap >= 0) {
            ray::SetTextureWrap(*dst, p.wrap);
        }
        if (p.filter >= 0) {
            ray::SetTextureFilter(*dst, p.filter);
        }
        ScreenStats& st = stats()[p.screen];
        ++st.uploaded;
        st.uploaded_bytes += p.bytes;
        st.upload_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
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

    void refill(const Texture* t, ray::Image image) {
        auto it = pending_map().find(t);
        if (it == pending_map().end() || !it->second.evicted || !image.data) {
            texture_retain::free_image_async(image);
            return;
        }
        Pending& p = it->second;
        if (image.width != t->width || image.height != t->height || image.format != t->format) {
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

    size_t refill_from(const std::string& owner, std::vector<std::pair<std::filesystem::path, ray::Image>>& images) {
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
                refill(texs[i], i + 1 < texs.size() ? ray::ImageCopy(image) : image);
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

    void set_decoder(ray::Image (*decode)(const std::filesystem::path& source)) {
        decoder = decode;
    }

    void log_screen(const std::string& screen_name) {
        for (auto& [screen, st] : stats()) {
            if (st.uploaded > 0) {
                spdlog::info("Lazy texture upload: {} pending textures of {} uploaded on first use since the last "
                             "screen load ({} MB, {:.1f} ms)",
                             st.uploaded, screen, st.uploaded_bytes / kMB, st.upload_ms);
                st.uploaded = 0;
                st.uploaded_bytes = 0;
                st.upload_ms = 0;
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
        st.eager = st.eager_bytes = st.lazy = st.lazy_bytes = 0;
        st.cap_logged = false;
    }

} // namespace texture_lazy
