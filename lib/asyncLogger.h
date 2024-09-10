#include <sstream>
#include <string>
#include <iostream>
#include "utils.h"

#define LOG(level) \
        LogCollector(level).stream()

class LogStream {
public:
    LogStream() = default;

    template <typename T>
    LogStream &operator<<(const T &t)
    {
        stream_ << t;
        return *this;
    }


    std::string str() const
    {
        return stream_.str();
    }

private:
    std::ostringstream stream_; 
};


class AsyncLogger {
public:
    AsyncLogger();
    ~AsyncLogger();
    void log(const std::string &msg);

private:
    void processQueue();
    ConcurrentQueue<std::string> logQueue_;
    std::thread worker_thread_;
    bool done_ = false;   
};

class LogCollector {
public:
    LogCollector(std::string level) : level_(level) {}

    ~LogCollector()
    {
        asyncLogger_->log(stream_.str());
    }

    LogStream &stream()
    {
        return stream_;
    }

private:
    std::string level_;
    LogStream stream_;
    std::unique_ptr<AsyncLogger> asyncLogger_ = std::make_unique<AsyncLogger>();
};

