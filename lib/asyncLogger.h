#include <sstream>
#include <string>
#include <iostream>
#include "utils.h"
#include <memory>

#define LOG(level) \
        LogCollector(LogLevel::level).stream()

#define VLOG(level) \
        LogCollector(LogLevel::INFO).stream()



enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR, 
    FATAL
};

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
    void flush();

private:
    void processQueue();
    ConcurrentQueue<std::string> logQueue_;
    std::thread worker_thread_;
    bool done_ = false;   
};

static std::shared_ptr<AsyncLogger> globalLogger_ = std::make_shared<AsyncLogger>(); 


class LogCollector {
public:
    // LogCollector(LogLevel level, std::shared_ptr<AsyncLogger> asyncLogger) : level_(level), asyncLogger_(asyncLogger) {}
    LogCollector(LogLevel level) : level_(level) {}

    ~LogCollector()
    {
        // globalLogger_->log(stream_.str());
        globalLogger_->log(stream_.str());
    }

    LogStream &stream()
    {
        return stream_;
    }

private:
    LogLevel level_;
    LogStream stream_;
    // std::shared_ptr<AsyncLogger> globalLogger_;
};
