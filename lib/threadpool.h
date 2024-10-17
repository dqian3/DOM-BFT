#ifndef VERIFICATION_MANAGER_H
#define VERIFICATION_MANAGER_H

#include "signature_provider.h"
#include <condition_variable>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

typedef std::function<void(byte *)> TaskFunc;
class ThreadPool;

struct Worker {
    // TODO not every worker needs a chunk of memory this big. Should we just allocate?
    std::thread thd;
    Worker(ThreadPool &pool);
};

// a class that handles node tasks.
class ThreadPool {
    friend Worker;

public:
    ThreadPool(size_t threadPoolSize);
    ~ThreadPool();

    // return a future object so that the result can be later retrieved
    void enqueueTask(TaskFunc &&f)
    {
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            tasks_.emplace(f);
        }
        condition_.notify_one();
    }

private:
    size_t threadPoolSize_;
    std::vector<Worker> workers_;

    // TODO use the concurrent queue we set up
    std::queue<TaskFunc> tasks_;
    std::mutex queueMutex_;
    std::condition_variable condition_;
    bool stop_;
};

#endif   // VERIFICATION_MANAGER_H
