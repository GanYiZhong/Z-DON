#include "zdon_os.h"

#include <cstdint>
#include <spdlog/spdlog.h>

#if defined(_WIN32)
#include "../platform/platform_windows.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__) // Android too
#include <cerrno>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__) // macOS and iOS
#include <mach/mach_time.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#endif

#ifdef ZDON_POSIX_LIST_DIR
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace zdon_os {

#if defined(__linux__)
    namespace {
        pid_t this_tid() {
            return static_cast<pid_t>(::syscall(SYS_gettid));
        }

        // a thread may go back to nice 0 after lowering it: the nice limit allows it (Android's init
        // raises it for apps; desktop distributions usually leave it at 0) or we are root
        bool nice_reversible() {
            static const bool ok = [] {
                struct rlimit rl{};
                return ::geteuid() == 0 ||
                       (::getrlimit(RLIMIT_NICE, &rl) == 0 && (rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur >= 20));
            }();
            return ok;
        }

        // the calling thread's nice value (as it started: a std::thread gets its creator's)
        int thread_nice() {
            errno = 0;
            const int n = ::getpriority(PRIO_PROCESS, static_cast<id_t>(this_tid()));
            return errno ? 0 : n;
        }
        void set_thread_nice(int n) {
            ::setpriority(PRIO_PROCESS, static_cast<id_t>(this_tid()), n < 19 ? n : 19);
        }
    } // namespace
#endif

    void background_thread() {
#if defined(_WIN32)
        win32_background_mode_thread();
#elif defined(__linux__)
        set_thread_nice(thread_nice() + 10); // per thread on Linux
#if !defined(__ANDROID__)
        // Best effort, level 7 (the lowest), not the idle class: an idle-class thread gets the disk only
        // when nobody else uses it, and the game sometimes waits for these reads (a predecode it takes).
        // Not on Android: ioprio_set is not in every app seccomp policy, and a blocked call kills the app.
        constexpr int kWhoProcess = 1; // IOPRIO_WHO_PROCESS; who = 0 is the calling thread
        constexpr int kClassBestEffort = 2;
        ::syscall(SYS_ioprio_set, kWhoProcess, 0, (kClassBestEffort << 13) | 7);
#endif
#elif defined(__APPLE__)
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
        setiopolicy_np(IOPOL_TYPE_DISK, IOPOL_SCOPE_THREAD, IOPOL_THROTTLE);
#endif
    }

    void job_priority_background(bool background) {
#if defined(_WIN32)
        SetThreadPriority(GetCurrentThread(), background ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
#elif defined(__linux__)
        if (nice_reversible()) {
            thread_local const int normal = thread_nice();
            set_thread_nice(background ? normal + 10 : normal);
        }
#elif defined(__APPLE__)
        // the thread's own class, saved before its first switch (a std::thread starts with its creator's)
        thread_local qos_class_t normal = [] {
            qos_class_t qc = QOS_CLASS_UNSPECIFIED;
            int rel = 0;
            pthread_get_qos_class_np(pthread_self(), &qc, &rel);
            return qc == QOS_CLASS_UNSPECIFIED ? QOS_CLASS_DEFAULT : qc;
        }();
        pthread_set_qos_class_self_np(background ? QOS_CLASS_UTILITY : normal, 0);
#else
        (void)background;
#endif
    }

    bool precise_sleep_us(long long us) {
#if defined(_WIN32)
        return win32_precise_sleep_us(us);
#elif defined(__APPLE__)
        static const mach_timebase_info_data_t tb = [] {
            mach_timebase_info_data_t t{};
            mach_timebase_info(&t);
            return t;
        }();
        if (tb.numer == 0 || tb.denom == 0) {
            return false;
        }
        if (us <= 0) {
            return true;
        }
        // ns -> ticks (125/3 ns per tick on Apple silicon, 1 on Intel)
        const auto ticks = static_cast<uint64_t>(static_cast<double>(us) * 1000.0 * tb.denom / tb.numer);
        mach_wait_until(mach_absolute_time() + ticks);
        return true;
#else
        (void)us;
        return false;
#endif
    }

    uint64_t physical_memory_mb() {
#if defined(_WIN32)
        MEMORYSTATUSEX m{};
        m.dwLength = sizeof(m);
        return GlobalMemoryStatusEx(&m) ? m.ullTotalPhys >> 20 : 0;
#elif defined(__linux__)
        const long pages = ::sysconf(_SC_PHYS_PAGES);
        const long page = ::sysconf(_SC_PAGESIZE);
        return pages > 0 && page > 0 ? (static_cast<uint64_t>(pages) * static_cast<uint64_t>(page)) >> 20 : 0;
#elif defined(__APPLE__)
        uint64_t bytes = 0;
        size_t len = sizeof(bytes);
        return ::sysctlbyname("hw.memsize", &bytes, &len, nullptr, 0) == 0 ? bytes >> 20 : 0;
#else
        return 0;
#endif
    }

    bool texture_extras_by_default() {
#if defined(__EMSCRIPTEN__)
        return false;
#else
        if constexpr (!kMobile) {
            return true;
        } else {
            static const bool big = [] {
                const uint64_t mb = physical_memory_mb();
                spdlog::info("Texture packs, lazy upload, predecode and retention: {} by default ({} MB of memory)",
                             mb >= 5632 ? "on" : "off", mb);
                return mb >= 5632;
            }();
            return big;
        }
#endif
    }

#ifdef ZDON_POSIX_LIST_DIR
    namespace {
        void set_stat(DirEntry& e, const struct stat& st) {
#if defined(__APPLE__)
            const struct timespec& t = st.st_mtimespec;
#else
            const struct timespec& t = st.st_mtim;
#endif
            constexpr int64_t kUnixEpochIn1601 = 11644473600; // seconds from 1601 to 1970
            e.size = static_cast<uint64_t>(st.st_size);
            e.ft_write = static_cast<uint64_t>((static_cast<int64_t>(t.tv_sec) + kUnixEpochIn1601) * 10000000 +
                                               static_cast<int64_t>(t.tv_nsec) / 100);
            e.stat_ok = true;
        }
    } // namespace

    bool list_dir(const std::filesystem::path& dir, std::vector<DirEntry>& out,
                  bool (*want_stat)(std::string_view name)) {
        DIR* d = ::opendir(dir.c_str());
        if (!d) {
            return false;
        }
        const int fd = ::dirfd(d);
        while (const struct dirent* de = ::readdir(d)) {
            const char* n = de->d_name;
            if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0'))) {
                continue;
            }
            DirEntry e;
            e.name = n;
            struct stat st{};
            unsigned char type = de->d_type;
            bool have_stat = false;
            if (type == DT_UNKNOWN) {
                if (::fstatat(fd, n, &st, AT_SYMLINK_NOFOLLOW) == 0) {
                    type = S_ISLNK(st.st_mode) ? DT_LNK : S_ISDIR(st.st_mode) ? DT_DIR : DT_REG;
                    have_stat = type != DT_LNK;
                }
            }
            if (type == DT_LNK) {
                e.is_link = true;
                have_stat = ::fstatat(fd, n, &st, 0) == 0; // the target (a broken link: not a folder)
                e.is_dir = have_stat && S_ISDIR(st.st_mode);
            } else {
                e.is_dir = type == DT_DIR;
            }
            if (!e.is_dir && want_stat && want_stat(e.name)) {
                if (!have_stat) {
                    have_stat = ::fstatat(fd, n, &st, 0) == 0;
                }
                if (have_stat) {
                    set_stat(e, st);
                }
            }
            out.push_back(std::move(e));
        }
        ::closedir(d);
        return true;
    }
#endif

} // namespace zdon_os
