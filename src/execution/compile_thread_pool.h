#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace umbra::execution {

/// A persistent thread pool for background LLVM compilation.
///
/// Workers wait on a condition variable for tasks. Submitting a task
/// is a notify (no pthread_create), so startup overhead is near zero.
/// Singleton — initialized lazily with hardware_concurrency() threads.
class CompileThreadPool {
   public:
    /// Get the global pool instance (created on first call).
    static CompileThreadPool& instance();

    /// Submit a task to the pool. Returns immediately.
    void submit(std::function<void()> task);

    // Non-copyable, non-movable.
    CompileThreadPool(const CompileThreadPool&) = delete;
    CompileThreadPool& operator=(const CompileThreadPool&) = delete;

    ~CompileThreadPool();

   private:
    explicit CompileThreadPool(unsigned numThreads);

    void workerLoop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool shutdown_{false};
};

}  // namespace umbra::execution
