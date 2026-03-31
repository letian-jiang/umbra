#include "execution/compile_thread_pool.h"

#include <algorithm>

namespace umbra::execution {

CompileThreadPool& CompileThreadPool::instance() {
    static CompileThreadPool pool(std::max(1u, std::thread::hardware_concurrency()));
    return pool;
}

CompileThreadPool::CompileThreadPool(unsigned numThreads) {
    workers_.reserve(numThreads);
    for (unsigned i = 0; i < numThreads; ++i) {
        workers_.emplace_back([this]() { workerLoop(); });
    }
}

CompileThreadPool::~CompileThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
    }
    cv_.notify_all();
    for (auto& w : workers_) {
        w.join();
    }
}

void CompileThreadPool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
}

void CompileThreadPool::workerLoop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return shutdown_ || !tasks_.empty(); });
            if (shutdown_ && tasks_.empty())
                return;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}

}  // namespace umbra::execution
