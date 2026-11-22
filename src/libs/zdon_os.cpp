#include "zdon_os.h"

#include <cstdint>
#include <map>
#include <spdlog/spdlog.h>
#include <string>

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

#if (defined(__linux__) || defined(__APPLE__)) && !defined(__EMSCRIPTEN__)
#include <sys/mman.h>
#include <sys/stat.h>
#endif
#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
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

    bool prefetch_file(const std::filesystem::path& path) {
#if defined(__linux__)
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd >= 0) {
            ::posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
            ::close(fd);
        }
        return true; // a file that does not open is not read the slow way either
#elif defined(__APPLE__)
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            return true;
        }
        struct stat st{};
        if (::fstat(fd, &st) == 0) {
            constexpr off_t kPiece = off_t{1} << 30;
            for (off_t off = 0; off < st.st_size; off += kPiece) {
                struct radvisory ra{};
                ra.ra_offset = off;
                ra.ra_count = static_cast<int>(st.st_size - off < kPiece ? st.st_size - off : kPiece);
                if (::fcntl(fd, F_RDADVISE, &ra) == -1) {
                    break;
                }
            }
        }
        ::close(fd);
        return true;
#else
        (void)path;
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
        // fs::directory_iterator rather than readdir: <dirent.h> resolves to raylib's Windows shim on this
        // project's include path. libstdc++ and libc++ keep readdir's d_type in each entry, so the type
        // costs no stat except for a link (or a file system that does not say).
        std::error_code ec;
        std::filesystem::directory_iterator iter(dir, ec);
        if (ec) {
            return false;
        }
        for (; !ec && iter != std::filesystem::directory_iterator(); iter.increment(ec)) {
            DirEntry e;
            e.name = iter->path().filename().string();
            std::error_code type_ec;
            e.is_link = iter->is_symlink(type_ec);
            e.is_dir = iter->is_directory(type_ec); // a link: its target (a broken link: not a folder)
            if (!e.is_dir && want_stat && want_stat(e.name)) {
                struct stat st{};
                if (::stat(iter->path().c_str(), &st) == 0) {
                    set_stat(e, st);
                }
            }
            out.push_back(std::move(e));
        }
        return true;
    }
#endif

    bool FileBytes::open(const std::filesystem::path& path, std::vector<unsigned char> (*read)(const char* path)) {
        clear();
#if defined(_WIN32)
        // FILE_SHARE_DELETE: the file can still be renamed while mapped (the skin updater moves it aside to
        // put a new version in place: skin_updater.cpp write_atomically)
        HANDLE f = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size{};
            if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (int64_t{1} << 31)) {
                HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
                if (m) {
                    view_ = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
                    CloseHandle(m); // the view keeps the mapping
                    if (view_) {
                        data_ = static_cast<const unsigned char*>(view_);
                        size_ = static_cast<size_t>(size.QuadPart);
                    }
                }
            }
            CloseHandle(f);
        }
#elif (defined(__linux__) || defined(__APPLE__)) && !defined(__EMSCRIPTEN__)
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            struct stat st{};
            if (::fstat(fd, &st) == 0 && st.st_size > 0) {
                void* v = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
                if (v != MAP_FAILED) {
                    view_ = v;
                    data_ = static_cast<const unsigned char*>(v);
                    size_ = static_cast<size_t>(st.st_size);
                }
            }
            ::close(fd);
        }
#endif
        if (!view_) {
            bytes_ = read(path.string().c_str());
            data_ = bytes_.data();
            size_ = bytes_.size();
        }
        return size_ > 0;
    }

    void FileBytes::clear() {
        if (view_) {
#if defined(_WIN32)
            UnmapViewOfFile(view_);
#elif (defined(__linux__) || defined(__APPLE__)) && !defined(__EMSCRIPTEN__)
            ::munmap(view_, size_);
#endif
            view_ = nullptr;
        }
        bytes_ = {};
        data_ = nullptr;
        size_ = 0;
    }

    void heap_histogram_log(const char* where) {
#if defined(_WIN32)
        struct Bucket {
            size_t n = 0, bytes = 0;
            std::string sample;
        };
        std::map<size_t, Bucket> by; // by size class (powers of two)
        size_t busy = 0;
        HANDLE heaps[64];
        const DWORD n = GetProcessHeaps(64, heaps);
        for (DWORD k = 0; k < n && k < 64; ++k) {
            if (!HeapLock(heaps[k])) {
                continue;
            }
            PROCESS_HEAP_ENTRY e{};
            while (HeapWalk(heaps[k], &e)) {
                if (!(e.wFlags & PROCESS_HEAP_ENTRY_BUSY)) {
                    continue;
                }
                size_t c = 16;
                while (c < e.cbData) {
                    c *= 2;
                }
                Bucket& b = by[c];
                ++b.n;
                b.bytes += e.cbData;
                busy += e.cbData;
                if (b.n % (c >= 8192 ? 41 : 997) == 1 &&
                    b.sample.size() < 2000u) { // a few blocks' first bytes, printable
                    const auto* p = static_cast<const unsigned char*>(e.lpData);
                    std::string t;
                    for (size_t j = 0; j < std::min<size_t>(e.cbData, c >= 8192 ? 96 : 64); ++j) {
                        t += (p[j] >= 32 && p[j] < 127) ? static_cast<char>(p[j]) : '.';
                    }
                    b.sample += " [" + t + "]";
                }
            }
            HeapUnlock(heaps[k]);
        }
        spdlog::info("[heap] {}: {:.1f} MB in use", where, busy / 1048576.0);
        for (const auto& [c, b] : by) {
            if (b.bytes >= (size_t{1} << 20)) {
                spdlog::info("[heap] {} <= {} B: {} blocks {:.1f} MB{}", where, c, b.n, b.bytes / 1048576.0, b.sample);
            }
        }
#else
        (void)where;
#endif
    }

} // namespace zdon_os
