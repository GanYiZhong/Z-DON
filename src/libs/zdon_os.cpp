#include "zdon_os.h"

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
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/resource.h>
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

} // namespace zdon_os
