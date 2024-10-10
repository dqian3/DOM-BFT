#include "threadpool.h"
#include <glog/logging.h>

ThreadPool::ThreadPool(size_t threadPoolSize)
    : threadPoolSize_(threadPoolSize)
    , stop_(false)
{
    for (size_t i = 0; i < threadPoolSize_; ++i) {
        workers_.emplace_back(&ThreadPool::workerThread, this);
    }
}

ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        stop_ = true;
    }
    condition_.notify_all();
    for (std::thread &worker : workers_) {
        worker.join();
    }
}

void ThreadPool::workerThread()
{
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            if (stop_ && tasks_.empty())
                return;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}
