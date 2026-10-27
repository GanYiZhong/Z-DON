#pragma once

// Z-DON: the operating system services the performance work uses, one call for every platform.
// Windows forwards to platform_windows; Linux/Android and macOS/iOS have their own versions; a
// platform without one gets a no-op (or false: the caller keeps its portable code).

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#if (defined(__linux__) || defined(__APPLE__)) && !defined(__EMSCRIPTEN__) // Android, iOS too
#define ZDON_POSIX_LIST_DIR 1 // list_dir below (Windows has win32_list_dir)
#endif

namespace zdon_os {

    // Background mode for the calling thread, for the rest of its life (threads started for
    // background work: read-ahead, texture pack readers/writers, freeing): lowest CPU and I/O priority.
    // Windows THREAD_MODE_BACKGROUND_BEGIN; Linux/Android nice 10 (and on Linux the lowest best-effort
    // I/O priority); macOS/iOS the utility QoS class and throttled disk I/O.
    void background_thread();

    // Lower (true) or normal (false) CPU priority for a thread that switches back and forth (the job
    // pool's workers). Windows below normal; macOS/iOS the utility QoS class. On Linux/Android only
    // when the thread may raise its nice value again (RLIMIT_NICE allows nice 0, or root): an
    // unprivileged thread can lower its priority but never undo it, and a worker stuck at a low
    // priority would run the game's own jobs there too. Elsewhere a no-op.
    void job_priority_background(bool background);

    // Sleeps for about `us` microseconds on a timer that wakes on time (the frame limiter and the input
    // thread spin the rest). Windows: a high-resolution waitable timer (plain sleeps round up to the
    // 15.6 ms tick). macOS/iOS: mach_wait_until, a hard deadline (nanosleep may be coalesced with other
    // timers by up to its leeway). Returns false elsewhere (Linux sleeps are already precise): the
    // caller then uses a normal sleep.
    bool precise_sleep_us(long long us);

    // Asks the kernel to read the whole file into the page cache in the background: Linux/Android
    // posix_fadvise(WILLNEED), macOS/iOS fcntl(F_RDADVISE) (an int count, so 1 GB per advisory).
    // false where there is no such hint: the caller reads the file itself.
    bool prefetch_file(const std::filesystem::path& path);

    // Physical memory in MB, 0 when it cannot be read.
    uint64_t physical_memory_mb();

#if defined(PLATFORM_ANDROID) || defined(YATAIDON_PLATFORM_IOS)
    constexpr bool kMobile = true;
#else
    constexpr bool kMobile = false;
#endif

    // Whether the memory-hungry texture work (texture packs, lazy upload, predecode, retention) is on
    // by default: always on desktop, never on the web (no threads, files in memory), on Android and
    // iOS only with at least 5.5 GB of physical memory (a "6 GB" phone reports a little less), and
    // there with smaller caps (mobile_cap_mb).
    bool texture_extras_by_default();
    // `desktop_mb` on desktop, `mobile_mb` on Android/iOS (the default cap of one of those features)
    constexpr long mobile_cap_mb(long desktop_mb, long mobile_mb) {
        return kMobile ? mobile_mb : desktop_mb;
    }

#ifdef ZDON_POSIX_LIST_DIR
    // One entry of a directory listing (the type comes with the name on the usual file systems).
    struct DirEntry {
        std::string name;
        bool is_dir = false;  // a link: whether its target is a folder (as fs::directory_entry::is_directory)
        bool is_link = false; // a symbolic link
        bool stat_ok = false; // size and ft_write are set
        uint64_t size = 0;
        // the last write as a Windows FILETIME (100 ns since 1601), the unit win32_list_dir gives:
        // the song cache stores it, so a library shared with Windows keeps its rows
        uint64_t ft_write = 0;
    };

    // Lists `dir` (without "." and "..") in fs::directory_iterator order (readdir's). Only an entry with an
    // unknown type (some file systems do not say) or a link is stat'ed for its type; size and ft_write (of
    // a link: its target's) are read, one stat, for the files `want_stat` accepts (null: none). Returns
    // false if the directory cannot be opened.
    bool list_dir(const std::filesystem::path& dir, std::vector<DirEntry>& out,
                  bool (*want_stat)(std::string_view name) = nullptr);
#endif

} // namespace zdon_os
