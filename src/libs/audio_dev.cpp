#include "audio_dev.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "spdlog/spdlog.h"

namespace audio_dev {
    namespace {
        bool env_on(const char* name) {
            const char* v = std::getenv(name);
            return v && *v && *v != '0';
        }

        // ---- stats: written by the callback with relaxed atomics, read and reset by the reporter ----
        constexpr int kBuckets = 4096; // 1 us each; the last one collects everything longer
        struct Stats {
            std::atomic<uint32_t> hist[kBuckets]{};
            std::atomic<uint64_t> count{0};
            std::atomic<uint64_t> sum_ns{0};
            std::atomic<int64_t> max_ns{0};
            std::atomic<int64_t> max_wait_ns{0};
            std::atomic<uint64_t> underflows{0};
            std::atomic<uint64_t> stream_underruns{0};
            std::atomic<int64_t> max_voices{0};
            std::atomic<uint64_t> poly_plays{0};
            std::atomic<uint64_t> poly_overlapped{0};
            std::atomic<uint64_t> poly_stolen{0};
        };
        Stats g_stats;

        void atomic_max(std::atomic<int64_t>& a, int64_t v) {
            int64_t cur = a.load(std::memory_order_relaxed);
            while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
            }
        }

        // ---- capture: SPSC float ring, callback -> writer thread ----
        struct Capture {
            std::vector<float> ring; // interleaved stereo
            size_t cap = 0;          // floats
            std::atomic<size_t> head{0}, tail{0};
            std::atomic<uint64_t> dropped{0};
            FILE* f = nullptr;
            uint64_t data_bytes = 0;
            unsigned int rate = 44100;
        };
        Capture g_cap;

        std::atomic<bool> g_running{false};
        std::thread g_stats_thread, g_cap_thread;

        void write_wav_header(FILE* f, unsigned int rate, uint64_t data_bytes) {
            auto u32 = [f](uint32_t v) { std::fwrite(&v, 4, 1, f); };
            auto u16 = [f](uint16_t v) { std::fwrite(&v, 2, 1, f); };
            const uint32_t data = (uint32_t)std::min<uint64_t>(data_bytes, 0xFFFFFFF0u);
            std::fseek(f, 0, SEEK_SET);
            std::fwrite("RIFF", 1, 4, f);
            u32(36 + data);
            std::fwrite("WAVEfmt ", 1, 8, f);
            u32(16);
            u16(3); // IEEE float
            u16(2);
            u32(rate);
            u32(rate * 8);
            u16(8);
            u16(32);
            std::fwrite("data", 1, 4, f);
            u32(data);
        }

        void drain_capture() {
            const size_t h = g_cap.head.load(std::memory_order_acquire);
            size_t t = g_cap.tail.load(std::memory_order_relaxed);
            if (h == t) {
                return;
            }
            std::fseek(g_cap.f, 0, SEEK_END);
            while (t != h) {
                const size_t i = t % g_cap.cap;
                const size_t n = std::min(h - t, g_cap.cap - i);
                std::fwrite(g_cap.ring.data() + i, sizeof(float), n, g_cap.f);
                g_cap.data_bytes += n * sizeof(float);
                t += n;
            }
            g_cap.tail.store(t, std::memory_order_release);
            write_wav_header(g_cap.f, g_cap.rate, g_cap.data_bytes);
            std::fflush(g_cap.f);
        }

        void report() {
            uint32_t hist[kBuckets];
            uint64_t n = 0;
            for (int i = 0; i < kBuckets; i++) {
                hist[i] = g_stats.hist[i].exchange(0, std::memory_order_relaxed);
                n += hist[i];
            }
            g_stats.count.exchange(0, std::memory_order_relaxed);
            const uint64_t sum = g_stats.sum_ns.exchange(0, std::memory_order_relaxed);
            const int64_t mx = g_stats.max_ns.exchange(0, std::memory_order_relaxed);
            const int64_t wait = g_stats.max_wait_ns.exchange(0, std::memory_order_relaxed);
            const uint64_t uf = g_stats.underflows.exchange(0, std::memory_order_relaxed);
            const uint64_t su = g_stats.stream_underruns.exchange(0, std::memory_order_relaxed);
            const int64_t voices = g_stats.max_voices.exchange(0, std::memory_order_relaxed);
            const uint64_t pp = g_stats.poly_plays.exchange(0, std::memory_order_relaxed);
            const uint64_t po = g_stats.poly_overlapped.exchange(0, std::memory_order_relaxed);
            const uint64_t ps = g_stats.poly_stolen.exchange(0, std::memory_order_relaxed);
            int p99 = 0;
            if (n > 0) {
                const uint64_t want = (n * 99 + 99) / 100;
                uint64_t acc = 0;
                for (int i = 0; i < kBuckets; i++) {
                    acc += hist[i];
                    if (acc >= want) {
                        p99 = i;
                        break;
                    }
                }
            }
            spdlog::info("[audio-stats] callbacks={} avg_us={:.1f} p99_us={} max_us={:.1f} max_lock_wait_us={:.1f} "
                         "underflows={} stream_underruns={} max_voices={} poly_plays={} overlapped={} stolen={}",
                         n, n ? (double)sum / (double)n / 1000.0 : 0.0, p99, (double)mx / 1000.0, (double)wait / 1000.0,
                         uf, su, voices, pp, po, ps);
        }
    } // namespace

    bool stats_enabled() {
        static const bool on = env_on("YATAIDON_AUDIO_STATS");
        return on;
    }

    bool capture_enabled() {
        static const bool on = std::getenv("YATAIDON_AUDIO_CAPTURE") && *std::getenv("YATAIDON_AUDIO_CAPTURE");
        return on;
    }

    const std::string& selftest_spec() {
        static const std::string spec = [] {
            const char* v = std::getenv("YATAIDON_AUDIO_SELFTEST");
            return std::string(v ? v : "");
        }();
        return spec;
    }

    void start(double sample_rate) {
        if (g_running.exchange(true)) {
            return;
        }
        if (capture_enabled()) {
            g_cap.rate = (unsigned int)sample_rate;
            g_cap.cap = (size_t)g_cap.rate * 2 * 8; // 8 s
            g_cap.ring.assign(g_cap.cap, 0.0f);
            g_cap.f = std::fopen(std::getenv("YATAIDON_AUDIO_CAPTURE"), "wb+");
            if (!g_cap.f) {
                spdlog::error("YATAIDON_AUDIO_CAPTURE: cannot open {}", std::getenv("YATAIDON_AUDIO_CAPTURE"));
                g_cap.cap = 0;
            } else {
                write_wav_header(g_cap.f, g_cap.rate, 0);
                g_cap_thread = std::thread([] {
                    while (g_running.load(std::memory_order_relaxed)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        drain_capture();
                    }
                });
            }
        }
        if (stats_enabled()) {
            g_stats_thread = std::thread([] {
                auto next = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (g_running.load(std::memory_order_relaxed)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    if (std::chrono::steady_clock::now() >= next) {
                        next += std::chrono::seconds(2);
                        report();
                    }
                }
            });
        }
    }

    void stop() {
        if (!g_running.exchange(false)) {
            return;
        }
        if (g_stats_thread.joinable()) {
            g_stats_thread.join();
            report();
        }
        if (g_cap_thread.joinable()) {
            g_cap_thread.join();
        }
        if (g_cap.f) {
            drain_capture();
            if (g_cap.dropped.load()) {
                spdlog::warn("YATAIDON_AUDIO_CAPTURE: dropped {} samples", g_cap.dropped.load());
            }
            std::fclose(g_cap.f);
            g_cap.f = nullptr;
            g_cap.cap = 0;
        }
    }

    void record_callback(int64_t mix_ns, int64_t lock_wait_ns) {
        const int64_t us = mix_ns / 1000;
        g_stats.hist[std::clamp<int64_t>(us, 0, kBuckets - 1)].fetch_add(1, std::memory_order_relaxed);
        g_stats.count.fetch_add(1, std::memory_order_relaxed);
        g_stats.sum_ns.fetch_add((uint64_t)std::max<int64_t>(mix_ns, 0), std::memory_order_relaxed);
        atomic_max(g_stats.max_ns, mix_ns);
        atomic_max(g_stats.max_wait_ns, lock_wait_ns);
    }

    void record_underflow() {
        g_stats.underflows.fetch_add(1, std::memory_order_relaxed);
    }

    void record_stream_underrun() {
        g_stats.stream_underruns.fetch_add(1, std::memory_order_relaxed);
    }

    void record_voices(size_t active) {
        atomic_max(g_stats.max_voices, (int64_t)active);
    }

    void record_poly_play(bool overlapped, bool stolen) {
        g_stats.poly_plays.fetch_add(1, std::memory_order_relaxed);
        g_stats.poly_overlapped.fetch_add(overlapped ? 1 : 0, std::memory_order_relaxed);
        g_stats.poly_stolen.fetch_add(stolen ? 1 : 0, std::memory_order_relaxed);
    }

    void capture(const float* stereo, unsigned int frames) {
        if (g_cap.cap == 0 || !g_running.load(std::memory_order_relaxed)) {
            return;
        }
        const size_t n = (size_t)frames * 2;
        size_t h = g_cap.head.load(std::memory_order_relaxed);
        const size_t t = g_cap.tail.load(std::memory_order_acquire);
        if (g_cap.cap - (h - t) < n) {
            g_cap.dropped.fetch_add(n, std::memory_order_relaxed);
            return;
        }
        for (size_t k = 0; k < n;) {
            const size_t i = h % g_cap.cap;
            const size_t m = std::min(n - k, g_cap.cap - i);
            std::memcpy(g_cap.ring.data() + i, stereo + k, m * sizeof(float));
            k += m;
            h += m;
        }
        g_cap.head.store(h, std::memory_order_release);
    }
} // namespace audio_dev
