#ifndef APPLICATION_H
#define APPLICATION_H

#include <memory>

#include "common_struct.h"

#include <google/protobuf/message.h>
#include <glog/logging.h>

// Originally had some custom classes here, but this is easier lol
typedef google::protobuf::Message AppRequest;
typedef google::protobuf::Message AppResponse;

enum class AppType
{
    KV_STORE,
    COUNTER
};

typedef struct AppLayerResponse
{
    std::unique_ptr<byte[]> response;
    bool success;
    size_t result_len;
} AppLayerResponse;

class Application
{

public:
    virtual ~Application() {};

    virtual std::unique_ptr<AppLayerResponse> execute(const std::string &serialized_request) = 0;

    virtual bool commit(uint32_t commit_idx, byte* committed_value) = 0;

    virtual byte* getDigest(uint32_t digest_idx) = 0;
    
    virtual byte* takeSnapshot() = 0;

    // resetting the application state to the committed state
    virtual bool abort() = 0;


};

class AppTrafficGen {
public:
    virtual ~AppTrafficGen() = default;

    // Virtual function to generate app traffic
    virtual void* generateAppTraffic() = 0;
};

#endif