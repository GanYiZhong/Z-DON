#include "jobs.h"

#include <algorithm>
#include <deque>
#include <thread>
#include <vector>

#include "zdon_os.h"

namespace jobs {
    namespace detail {
        void Task::run() {
            try {
                fn();
            } catch (...) {
                error = std::current_exception();
            }
            fn = nullptr; // drop the captures here, not wherever the last handle goes
            {
                std::scoped_lock lock(mutex);
                state.store(2, std::memory_order_release);
            }
            cv.notify_all();
        }
    } // namespace detail

    namespace {
        unsigned pool_size() {
            const unsigned hw = std::thread::hardware_concurrency();
            return std::clamp(hw > 1 ? hw - 1 : 2u, 2u, 64u);
        }

#ifdef __EMSCRIPTEN__
        constexpr bool kThreads = false;
#else
        constexpr bool kThreads = true;
#endif

        class Pool {
        public:
            explicit Pool(unsigned n) : size(n), background_limit(std::max(1u, n - 1)) {
                for (unsigned i = 0; i < n; ++i) {
                    std::thread([this] { work(); }).detach(); // never joined: the pool lives until exit
                }
            }

            void push(std::shared_ptr<detail::Task> task) {
                {
                    std::scoped_lock lock(mutex);
                    queues[static_cast<size_t>(task->priority)].push_back(std::move(task));
                }
                cv.notify_one();
            }

            const unsigned size;

        private:
            // the next job a worker may take, or null (call with the lock held)
            std::shared_ptr<detail::Task> pop() {
                for (size_t p = 0; p < 3; ++p) {
                    auto& q = queues[p];
                    // jobs a waiter already ran: drop them
                    while (!q.empty() && q.front()->state.load(std::memory_order_acquire) != 0) {
                        q.pop_front();
                    }
                    if (q.empty()) {
                        continue;
                    }
                    if (p == static_cast<size_t>(Priority::Background) && running_background >= background_limit) {
                        return nullptr;
                    }
                    auto task = std::move(q.front());
                    q.pop_front();
                    return task;
                }
                return nullptr;
            }

            static void set_background(bool background) {
                zdon_os::job_priority_background(background); // see there for Linux
            }

            void work() {
                bool background = false;
                std::unique_lock<std::mutex> lock(mutex);
                while (true) {
                    std::shared_ptr<detail::Task> task;
                    cv.wait(lock, [&] { return (task = pop()) != nullptr; });
                    const bool bg = task->priority == Priority::Background;
                    if (bg) {
                        ++running_background;
                    }
                    lock.unlock();
                    if (task->claim()) {
                        if (bg != background) {
                            set_background(bg);
                            background = bg;
                        }
                        task->run();
                    }
                    task.reset();
                    lock.lock();
                    if (bg) {
                        --running_background;
                        cv.notify_one(); // a background job may be waiting for this slot
                    }
                }
            }

            std::mutex mutex;
            std::condition_variable cv;
            std::deque<std::shared_ptr<detail::Task>> queues[3];
            unsigned running_background = 0;
            const unsigned background_limit;
        };

        Pool& pool() {
            static Pool* p = new Pool(pool_size()); // leaked on purpose: no join at static destruction
            return *p;
        }
    } // namespace

    void Handle::wait() {
        if (!task) {
            return;
        }
        if (task->claim()) {
            task->run();
            return;
        }
        std::unique_lock<std::mutex> lock(task->mutex);
        task->cv.wait(lock, [&] { return task->state.load(std::memory_order_acquire) == 2; });
    }

    void Handle::get() {
        wait();
        std::shared_ptr<detail::Task> t = std::move(task);
        if (t && t->error) {
            std::rethrow_exception(t->error);
        }
    }

    Handle submit(Priority priority, std::function<void()> fn) {
        Handle h;
        h.task = std::make_shared<detail::Task>();
        h.task->fn = std::move(fn);
        h.task->priority = priority;
        if (kThreads) {
            pool().push(h.task);
        } else {
            h.wait();
        }
        return h;
    }

    void parallel_for(size_t n, Priority priority, const std::function<void(size_t)>& fn, size_t max_threads) {
        if (n == 0) {
            return;
        }
        const size_t threads = std::min(n, max_threads ? max_threads : static_cast<size_t>(worker_count()) + 1);
        std::atomic<size_t> next{0};
        auto loop = [&] {
            for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < n;) {
                fn(i);
            }
        };
        std::vector<Handle> helpers;
        helpers.reserve(threads > 0 ? threads - 1 : 0);
        for (size_t k = 1; k < threads; ++k) {
            helpers.push_back(submit(priority, loop));
        }
        std::exception_ptr error;
        try {
            loop();
        } catch (...) {
            error = std::current_exception();
            next.store(n, std::memory_order_relaxed); // the helpers stop after their current call
        }
        for (Handle& h : helpers) {
            h.wait(); // a helper no worker has started runs here, finds nothing left and returns
        }
        if (error) {
            std::rethrow_exception(error);
        }
        for (Handle& h : helpers) {
            h.get();
        }
    }

    unsigned worker_count() {
        return kThreads ? pool().size : 0;
    }
} // namespace jobs
