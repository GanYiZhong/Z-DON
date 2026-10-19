#pragma once

// Z-DON: one shared pool of worker threads for short CPU jobs (decoding, parsing, text rasterizing),
// instead of every caller starting its own threads.
//
// - Workers: hardware_concurrency() - 1 (at least 2, at most 64), started on the first job.
// - Three priorities. A free worker takes the oldest Critical job, then Normal, then Background.
//   Background jobs run below normal thread priority and never take the last worker, so they
//   cannot hold up foreground work.
// - Handle::wait() runs the job on the waiting thread when no worker has started it yet. A wait
//   therefore never depends on a free worker: it either runs the job itself or waits for a job
//   that is already running, so a job may wait on other jobs (as long as the waits form no cycle).
// - Jobs must not block on anything other than other jobs (no network, no waiting for the main
//   thread): a blocked job holds a worker.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>

namespace jobs {
    enum class Priority : unsigned char {
        Critical,   // the current screen is waiting for it
        Normal,     // needed soon (text for a box that just appeared, speculative loads)
        Background, // nobody waits for it (library scans)
    };

    namespace detail {
        struct Task {
            std::function<void()> fn;
            Priority priority = Priority::Normal;
            std::atomic<int> state{0}; // 0 queued, 1 running, 2 done
            std::exception_ptr error;
            std::mutex mutex;
            std::condition_variable cv;

            void run(); // only by whoever moved the state from queued to running
            bool claim() {
                int expected = 0;
                return state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel);
            }
        };
    } // namespace detail

    class Handle {
    public:
        Handle() = default;
        bool valid() const { return task != nullptr; }
        bool done() const { return task && task->state.load(std::memory_order_acquire) == 2; }
        // runs the job here when no worker has started it, else waits for it to finish
        void wait();
        // wait(), then rethrows what the job threw; the handle is empty afterwards
        void get();

    private:
        friend Handle submit(Priority, std::function<void()>);
        std::shared_ptr<detail::Task> task;
    };

    Handle submit(Priority priority, std::function<void()> fn);

    // fn(i) for every i in [0, n), on up to max_threads threads (0: all workers) including the
    // calling thread, which takes part; returns when every call has returned. Indices are handed
    // out in order from one counter, as the per-call thread pools this replaces did.
    void parallel_for(size_t n, Priority priority, const std::function<void(size_t)>& fn, size_t max_threads = 0);

    unsigned worker_count();
} // namespace jobs
