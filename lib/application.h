#ifndef APPLICATION_H
#define APPLICATION_H

#include <memory>
#include <fstream>
#include <yaml-cpp/yaml.h>

#include "common.h"

#include <glog/logging.h>
#include <google/protobuf/message.h>


// Originally had some custom classes here, but this is easier lol
typedef google::protobuf::Message AppRequest;
typedef google::protobuf::Message AppResponse;

enum class AppType { KV_STORE, COUNTER };

typedef struct AppLayerResponse {
    std::unique_ptr<byte[]> response;
    bool success;
    size_t result_len;
} AppLayerResponse;

class Application {

public:
    virtual ~Application(){};

    virtual std::string execute(const std::string &serialized_request, const uint32_t execute_idx) = 0;

    virtual bool commit(uint32_t commit_idx) = 0;
    // resetting the application state to the committed state
    virtual bool abort(const uint32_t abort_idx) = 0;

    virtual std::string getDigest(uint32_t digest_idx) = 0;

    // take a snapshot of the latest application state
    virtual bool takeSnapshot() = 0;

    virtual std::string getSnapshot(uint32_t seq) = 0;

    virtual void applySnapshot(const std::string &snapshot) = 0;

    // Store the application state in a YAML file
    // This may include state metadata and the actual App data
    virtual void storeAppStateInYAML(const std::string& filename) = 0;
};

class AppTrafficGen {
public:
    virtual ~AppTrafficGen() = default;

    // Virtual function to generate app traffic
    virtual void *generateAppTraffic() = 0;
};

#endif