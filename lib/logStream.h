#include <sstream>
#include <string>
#include <iostream>

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
}