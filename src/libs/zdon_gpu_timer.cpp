#include "zdon_gpu_timer.h"

#include <SDL3/SDL.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <spdlog/spdlog.h>

#include "raylib_ns.h"

using ray::Matrix;
#include <rlgl.h>

namespace zdon_gpu_timer {
    namespace {
        using GenQueries = void (*)(int, unsigned*);
        using QueryCounter = void (*)(unsigned, unsigned);
        using GetQueryObjectiv = void (*)(unsigned, unsigned, int*);
        using GetQueryObjectui64v = void (*)(unsigned, unsigned, uint64_t*);
        constexpr unsigned kTimestamp = 0x8E28, kResult = 0x8866, kAvailable = 0x8867;
        constexpr int kRing = 4;

        GenQueries gen = nullptr;
        QueryCounter counter = nullptr;
        GetQueryObjectiv get_iv = nullptr;
        GetQueryObjectui64v get_u64 = nullptr;
        std::array<std::array<unsigned, kMarks>, kRing> q{};
        std::array<bool, kRing> pending{};
        std::array<uint64_t, kRing> first_ns{}; // FRAME_START of each slot, for start -> start
        int slot = 0;
        bool ready = false;

        double sum[kMarks] = {};
        double frame_sum = 0;
        int frames = 0;
        uint64_t prev_start = 0;
        auto since = std::chrono::steady_clock::now();

        bool init() {
            gen = reinterpret_cast<GenQueries>(SDL_GL_GetProcAddress("glGenQueries"));
            counter = reinterpret_cast<QueryCounter>(SDL_GL_GetProcAddress("glQueryCounter"));
            get_iv = reinterpret_cast<GetQueryObjectiv>(SDL_GL_GetProcAddress("glGetQueryObjectiv"));
            get_u64 = reinterpret_cast<GetQueryObjectui64v>(SDL_GL_GetProcAddress("glGetQueryObjectui64v"));
            if (!gen || !counter || !get_iv || !get_u64) {
                spdlog::warn("GPU timer: no timestamp queries");
                return false;
            }
            for (auto& s : q) {
                gen(kMarks, s.data());
            }
            spdlog::info("GPU timer: on");
            return true;
        }

        void collect(int s, bool wait) {
            int avail = 0;
            get_iv(q[s][PRESENT], kAvailable, &avail);
            if (!avail && !wait) {
                return; // still running: looked at again next time round
            }
            uint64_t t[kMarks];
            for (int m = 0; m < kMarks; ++m) {
                get_u64(q[s][m], kResult, &t[m]);
            }
            pending[s] = false;
            for (int m = 1; m < kMarks; ++m) {
                sum[m] += static_cast<double>(t[m] - t[m - 1]) / 1e6;
            }
            if (prev_start != 0 && t[FRAME_START] > prev_start) {
                frame_sum += static_cast<double>(t[FRAME_START] - prev_start) / 1e6;
            }
            prev_start = t[FRAME_START];
            ++frames;
        }
    } // namespace

    bool on() {
        static const bool v = [] {
            const char* e = std::getenv("ZDON_GPU_TIMER");
            return e && *e == '1';
        }();
        return v;
    }

    void mark(Mark m) {
        if (!on()) {
            return;
        }
        if (!ready) {
            static bool tried = false;
            if (tried) {
                return;
            }
            tried = true;
            ready = init();
            if (!ready) {
                return;
            }
        }
        if (m == FRAME_START && pending[slot]) {
            collect(slot, true); // the ring came round: this slot's last frame is read before its queries are reused
        }
        ray::rlDrawRenderBatchActive(); // the GPU work drawn so far is before this mark
        counter(q[slot][m], kTimestamp);
    }

    void frame_end() {
        if (!on() || !ready) {
            return;
        }
        pending[slot] = true;
        slot = (slot + 1) % kRing;
        for (int s = 0; s < kRing; ++s) {
            if (pending[s] && s != slot) {
                collect(s, false);
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - since).count() >= 2.0 && frames > 0) {
            spdlog::info("[gpu] {} frames, GPU ms per frame: start->draw {:.3f}, screen draw {:.3f}, "
                         "overlay->present {:.3f}; frame start to next start {:.3f}",
                         frames, sum[DRAW_START] / frames, sum[DRAW_END] / frames, sum[PRESENT] / frames,
                         frames > 1 ? frame_sum / (frames - 1) : 0.0);
            for (double& v : sum) {
                v = 0;
            }
            frame_sum = 0;
            frames = 0;
            since = now;
        }
    }
} // namespace zdon_gpu_timer
