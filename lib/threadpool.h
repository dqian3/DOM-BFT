#ifndef VERIFICATION_MANAGER_H
#define VERIFICATION_MANAGER_H

#include "signature_provider.h"
#include <condition_variable>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
// #include <cstddef>
// #include <set>
// #include <string>

typedef std::function<void(byte *)> TaskFunc;
class ThreadPool;

struct Worker {
    // TODO not every worker needs a chunk of memory this big. Should we just allocate?
    byte buffer[SEND_BUFFER_SIZE];
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
    template <typename F> auto enqueueTask(F &&f) -> std::future<decltype(f())>
    {
        auto task = std::make_shared<std::packaged_task<decltype(f())()>>(std::forward<F>(f));
        std::future<decltype(f())> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            tasks_.emplace([task]() { (*task)(); });
        }
        condition_.notify_one();
        return res;
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
