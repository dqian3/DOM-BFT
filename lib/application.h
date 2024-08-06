#ifndef APPLICATION_H
#define APPLICATION_H

#include <memory>

#include "common_struct.h"

#include <google/protobuf/message.h>

// Originally had some custom classes here, but this is easier lol
typedef google::protobuf::Message AppRequest;
typedef google::protobuf::Message AppResponse;

class Application
{

public:
    virtual ~Application() {};

    virtual std::unique_ptr<AppResponse> execute(const AppRequest &request) = 0;

    // virtual uint32_t takeSnapshot() = 0;
    // virtual bool restoreSnapshot() = 0;

    virtual std::string execute(const std::string &serialized_request) = 0;

    virtual bool commit(uint32_t commit_idx) = 0;

    // TODO I am uncertain whether to use a unique pointer here. 
    virtual std::unique_ptr<byte[]> getDigest(uint32_t digest_idx) = 0;

    virtual std::unique_ptr<byte[]> takeSnapshot() = 0;

    // resetting the application state to the committeed state.
    virtual bool abort() = 0;

};

class AppTrafficGen {
public:
    virtual ~AppTrafficGen() = default;

    // Virtual function to generate app traffic
    virtual void* generateAppTraffic() = 0;
};

#endif