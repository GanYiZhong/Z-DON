#include "text_stats.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>

#include "spdlog/spdlog.h"

namespace text_stats {
    namespace {
        struct Bucket {
            long count = 0;
            double total_ms = 0.0;
            double max_ms = 0.0;
        };
        std::mutex g_mutex;
        Bucket g_buckets[KIND_COUNT];
        std::chrono::steady_clock::time_point g_window{};
    } // namespace

    bool enabled() {
        static const bool on = [] {
            const char* v = std::getenv("YATAIDON_TEXT_STATS");
            return v && *v == '1';
        }();
        return on;
    }

    void record(Kind kind, double ms) {
        std::scoped_lock lock(g_mutex);
        const auto now = std::chrono::steady_clock::now();
        if (g_window == std::chrono::steady_clock::time_point{}) {
            g_window = now;
        }
        Bucket& b = g_buckets[kind];
        b.count++;
        b.total_ms += ms;
        b.max_ms = std::max(b.max_ms, ms);
        if (now - g_window < std::chrono::seconds(2)) {
            return;
        }
        const Bucket& c = g_buckets[CTOR];
        const Bucket& r = g_buckets[REGISTER];
        const Bucket& a = g_buckets[ATLAS];
        const Bucket& u = g_buckets[UPLOAD];
        spdlog::info("[text] ctor {} {:.3f}/{:.3f} ms, register {} {:.3f}/{:.3f} ms, atlas {} {:.3f}/{:.3f} ms, "
                     "upload {} {:.3f}/{:.3f} ms (count total/max)",
                     c.count, c.total_ms, c.max_ms, r.count, r.total_ms, r.max_ms, a.count, a.total_ms, a.max_ms,
                     u.count, u.total_ms, u.max_ms);
        for (Bucket& x : g_buckets) {
            x = {};
        }
        g_window = now;
    }
} // namespace text_stats
