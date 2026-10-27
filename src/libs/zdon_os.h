#pragma once

// Z-DON: the operating system services the performance work uses, one call for every platform.
// Windows forwards to platform_windows; Linux/Android and macOS/iOS have their own versions; a
// platform without one gets a no-op (or false: the caller keeps its portable code).

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

} // namespace zdon_os
