#include "asyncLogger.h"    

AsyncLogger::AsyncLogger()
{
    worker_thread_ = std::thread(&AsyncLogger::processQueue, this);
}   

AsyncLogger::~AsyncLogger()
{
    done_ = true;
    worker_thread_.join();
}

void AsyncLogger::log(const std::string &msg)
{
    logQueue_.enqueue(msg);
}

void AsyncLogger::flush()
{
    processQueue();
}


void AsyncLogger::processQueue()
{
    while (!done_)
    {
        std::string msg;
        if (logQueue_.try_dequeue(msg))
        {
            std::cout << "Async LOG lib: "<< msg << std::endl;
        }
        // std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}