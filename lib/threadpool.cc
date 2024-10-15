#include "threadpool.h"
#include <glog/logging.h>

Worker::Worker(ThreadPool &pool)
{
    thd = std::thread([&] {
        byte *buffer = ((byte *) malloc(SEND_BUFFER_SIZE));

        while (true) {
            TaskFunc task;
            {
                std::unique_lock<std::mutex> lock(pool.queueMutex_);
                pool.condition_.wait(lock, [&] { return pool.stop_ || !pool.tasks_.empty(); });
                if (pool.stop_ && pool.tasks_.empty())
                    return;
                task = std::move(pool.tasks_.front());
                pool.tasks_.pop();
            }
            task(buffer);
        }
    });
}

ThreadPool::ThreadPool(size_t threadPoolSize)
    : threadPoolSize_(threadPoolSize)
    , stop_(false)
{
    for (size_t i = 0; i < threadPoolSize_; ++i) {
        workers_.emplace_back(Worker(*this));
    }
}

ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        stop_ = true;
    }
    condition_.notify_all();
    for (Worker &worker : workers_) {
        worker.thd.join();
    }
}
