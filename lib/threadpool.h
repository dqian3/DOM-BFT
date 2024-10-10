#ifndef VERIFICATION_MANAGER_H
#define VERIFICATION_MANAGER_H

#include "proto/dombft_proto.pb.h"
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

// a class that handles all the verification stuff.
class ThreadPool {
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
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex queueMutex_;
    std::condition_variable condition_;
    bool stop_;

    void workerThread();
};

#endif   // VERIFICATION_MANAGER_H
