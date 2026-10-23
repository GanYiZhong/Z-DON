#include "texture_retain.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <spdlog/spdlog.h>
#include <thread>
#include <unordered_map>
#include <vector>

#include "sdl_compat.h"
#include "texture.h"

#ifdef _WIN32
#include "../platform/platform_windows.h"
#define ZDON_GLAPI __stdcall
#else
#define ZDON_GLAPI
#endif

namespace texture_retain {
    namespace {

        constexpr size_t kMB = size_t{1} << 20;

        // The driver's dedicated video memory in MB, or 0 when it does not say.
        size_t reported_vram_mb() {
#if defined(PLATFORM_ANDROID) || defined(YATAIDON_PLATFORM_IOS) || defined(__EMSCRIPTEN__)
            return 0;
#else
            using GetIntegerv = void(ZDON_GLAPI*)(unsigned int, int*);
            auto get = reinterpret_cast<GetIntegerv>(SDL_GL_GetProcAddress("glGetIntegerv"));
            if (!get) {
                return 0;
            }
            if (SDL_GL_ExtensionSupported("GL_NVX_gpu_memory_info")) {
                int kb = 0;
                get(0x9047, &kb); // GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX
                return kb > 0 ? static_cast<size_t>(kb) / 1024 : 0;
            }
            if (SDL_GL_ExtensionSupported("GL_ATI_meminfo")) {
                int info[4] = {0, 0, 0, 0};
                get(0x87FC, info); // TEXTURE_FREE_MEMORY_ATI: free pool kB first
                return info[0] > 0 ? static_cast<size_t>(info[0]) / 1024 : 0;
            }
            return 0;
#endif
        }

        size_t budget_bytes() {
            static const size_t budget = [] {
                if (const char* v = std::getenv("YATAIDON_RETAIN_MB"); v && *v) {
                    const long mb = std::strtol(v, nullptr, 10);
                    return mb > 0 ? static_cast<size_t>(mb) * kMB : size_t{0};
                }
#if defined(PLATFORM_ANDROID) || defined(YATAIDON_PLATFORM_IOS) || defined(__EMSCRIPTEN__)
                return size_t{0};
#else
                const size_t vram = reported_vram_mb();
                const size_t mb = vram ? std::clamp<size_t>(vram / 4, 256, 1024) : 512;
                spdlog::info("Texture retention budget: {} MB (video memory reported: {} MB)", mb, vram);
                return mb * kMB;
#endif
            }();
            return budget;
        }

        struct Snapshot {
            int width = 0;
            int height = 0;
            std::vector<int> x, y, x2, y2;
            std::optional<std::vector<Rect>> crop_data;
            float opaque_right = 0.0f;
            std::vector<std::shared_ptr<Texture>> frames; // FramedTexture order
        };

        struct Entry {
            std::string screen;
            std::shared_ptr<TextureObject> obj;
            Snapshot snap;
        };

        struct ScreenSet {
            size_t bytes = 0;
            size_t objects = 0;
            size_t reused = 0;
            uint64_t last_use = 0;
            bool complete = true;
            bool refused_logged = false;
        };

        std::unordered_map<std::string, Entry> entries;
        std::unordered_map<std::string, ScreenSet> screens;
        // texture -> retained objects using it (by object, not GL id: a texture waiting for its first use,
        // texture_lazy.h, has id 0 and counts as if uploaded)
        std::unordered_map<const Texture*, size_t> gpu_refs;
        size_t total_bytes = 0;
        uint64_t use_clock = 0;

        size_t texture_bytes(const Texture& t) {
            return static_cast<size_t>(ray::GetPixelDataSize(t.width, t.height, t.format));
        }

        std::vector<const Texture*> gpu_textures(const TextureObject& obj) {
            std::vector<const Texture*> out;
            for (int i = 0; i < obj.frame_count(); ++i) {
                const Texture* t = obj.frame_texture_raw(i); // must not upload
                if (t && std::ranges::find(out, t) == out.end()) {
                    out.push_back(t);
                }
            }
            return out;
        }

        // bytes the object's textures would add (those no retained object holds yet)
        size_t new_bytes(const TextureObject& obj) {
            size_t bytes = 0;
            for (const Texture* t : gpu_textures(obj)) {
                if (!gpu_refs.contains(t)) {
                    bytes += texture_bytes(*t);
                }
            }
            return bytes;
        }

        void release(const Entry& e) {
            for (const Texture* t : gpu_textures(*e.obj)) {
                auto it = gpu_refs.find(t);
                if (it != gpu_refs.end() && --it->second == 0) {
                    gpu_refs.erase(it);
                    total_bytes -= std::min(total_bytes, texture_bytes(*t));
                }
            }
        }

        void drop_screen(const std::string& screen) {
            size_t freed = total_bytes;
            for (auto it = entries.begin(); it != entries.end();) {
                if (it->second.screen == screen) {
                    release(it->second);
                    it = entries.erase(it);
                } else {
                    ++it;
                }
            }
            screens.erase(screen);
            spdlog::info("Retained textures for {} dropped ({} MB, over budget)", screen, (freed - total_bytes) / kMB);
        }

        Snapshot snapshot(const TextureObject& obj) {
            Snapshot s;
            s.width = obj.width;
            s.height = obj.height;
            s.x = obj.x;
            s.y = obj.y;
            s.x2 = obj.x2;
            s.y2 = obj.y2;
            s.crop_data = obj.crop_data;
            s.opaque_right = obj.opaque_right;
            if (auto* framed = dynamic_cast<const FramedTexture*>(&obj)) {
                s.frames = framed->textures;
            }
            return s;
        }

        void restore(TextureObject& obj, const Snapshot& s) {
            obj.width = s.width;
            obj.height = s.height;
            obj.x = s.x;
            obj.y = s.y;
            obj.x2 = s.x2;
            obj.y2 = s.y2;
            obj.crop_data = s.crop_data;
            obj.opaque_right = s.opaque_right;
            if (auto* framed = dynamic_cast<FramedTexture*>(&obj)) {
                framed->textures = s.frames;
            }
        }

        // Decoded images freed on a background thread. Never destroyed: the thread may still be
        // waiting when the process exits.
        class FreeQueue {
        public:
            static constexpr size_t kMaxPending = size_t{512} << 20;

            // false when too much is waiting already (the caller frees it)
            bool push(void* data, size_t bytes) {
                {
                    std::scoped_lock lock(mutex);
                    if (pending_bytes + bytes > kMaxPending) {
                        return false;
                    }
                    if (!started) {
                        started = true;
                        std::thread([this] { run(); }).detach();
                    }
                    queue.emplace_back(data, bytes);
                    pending_bytes += bytes;
                }
                cv.notify_one();
                return true;
            }

        private:
            void run() {
#ifdef _WIN32
                win32_background_mode_thread();
#endif
                std::unique_lock<std::mutex> lock(mutex);
                while (true) {
                    cv.wait(lock, [this] { return !queue.empty(); });
                    auto [data, bytes] = queue.front();
                    queue.pop_front();
                    lock.unlock();
                    ray::Image img{};
                    img.data = data;
                    ray::UnloadImage(img);
                    lock.lock();
                    pending_bytes -= bytes;
                }
            }

            std::mutex mutex;
            std::condition_variable cv;
            std::deque<std::pair<void*, size_t>> queue;
            size_t pending_bytes = 0;
            bool started = false;
        };

    } // namespace

    bool wanted(const std::string& screen_name) {
        return (screen_name == "song_select" || screen_name == "dan_select") && budget_bytes() > 0;
    }

    void add(const std::string& screen_name, const std::string& key, const std::shared_ptr<TextureObject>& obj) {
        if (!obj || !wanted(screen_name)) {
            return;
        }
        if (auto old = entries.find(key); old != entries.end()) {
            release(old->second);
            entries.erase(old);
        }
        const size_t bytes = new_bytes(*obj);
        while (total_bytes + bytes > budget_bytes()) {
            // the least recently used other screen goes first
            const std::string* lru = nullptr;
            uint64_t oldest = UINT64_MAX;
            for (const auto& [name, set] : screens) {
                if (name != screen_name && set.last_use < oldest) {
                    oldest = set.last_use;
                    lru = &name;
                }
            }
            if (!lru) {
                break;
            }
            drop_screen(std::string(*lru));
        }
        ScreenSet& set = screens[screen_name];
        set.last_use = ++use_clock;
        if (total_bytes + bytes > budget_bytes()) {
            set.complete = false;
            if (!set.refused_logged) {
                set.refused_logged = true;
                spdlog::info("Texture retention budget ({} MB) full: the rest of {} loads from disk next time",
                             budget_bytes() / kMB, screen_name);
            }
            return;
        }
        for (const Texture* t : gpu_textures(*obj)) {
            ++gpu_refs[t];
        }
        total_bytes += bytes;
        set.bytes += bytes;
        ++set.objects;
        entries.emplace(key, Entry{screen_name, obj, snapshot(*obj)});
    }

    std::shared_ptr<TextureObject> reuse(const std::string& key) {
        auto it = entries.find(key);
        if (it == entries.end()) {
            return nullptr;
        }
        // Held only here: put it back as loaded. Still in use elsewhere (the folder loaded twice
        // while its screen runs): leave it as it is, as the load_folder cache always did.
        if (it->second.obj.use_count() == 1) {
            restore(*it->second.obj, it->second.snap);
        }
        ScreenSet& set = screens[it->second.screen];
        set.last_use = ++use_clock;
        ++set.reused;
        return it->second.obj;
    }

    bool contains(const std::string& key) {
        return entries.contains(key);
    }

    bool screen_complete(const std::string& screen_name) {
        auto it = screens.find(screen_name);
        return it != screens.end() && it->second.complete && it->second.objects > 0;
    }

    void log_screen(const std::string& screen_name) {
        auto it = screens.find(screen_name);
        if (it == screens.end()) {
            return;
        }
        ScreenSet& set = it->second;
        set.refused_logged = false;
        spdlog::info("Retained textures for {}: {} objects, {} MB, {} reused (all screens {} of {} MB)", screen_name,
                     set.objects, set.bytes / kMB, set.reused, total_bytes / kMB, budget_bytes() / kMB);
        set.reused = 0;
    }

    void clear() {
        entries.clear();
        screens.clear();
        gpu_refs.clear();
        total_bytes = 0;
    }

    void free_image_async(ray::Image& image) {
        if (!image.data) {
            return;
        }
#ifdef __EMSCRIPTEN__
        ray::UnloadImage(image);
#else
        static FreeQueue* queue = new FreeQueue();
        const size_t bytes = static_cast<size_t>(ray::GetPixelDataSize(image.width, image.height, image.format));
        if (!queue->push(image.data, bytes)) {
            ray::UnloadImage(image);
        }
#endif
        image.data = nullptr;
    }

} // namespace texture_retain
