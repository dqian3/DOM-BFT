#ifndef APPLICATION_H
#define APPLICATION_H

#include <fstream>
#include <memory>
#include <yaml-cpp/yaml.h>

#include "common.h"

#include <glog/logging.h>
#include <google/protobuf/message.h>

enum class AppType { KV_STORE, COUNTER };

struct AppSnapshot {
    uint32_t idx;

    std::string snapshot;
    std::string digest;
};

class Application {

public:
    virtual ~Application() {};

    // Execute the request and return the serialized response
    virtual std::string execute(const std::string &serialized_request, uint32_t execute_idx) = 0;

    // Cleanup any unecessary metadata for rolling back to any state before or including commit_idx
    virtual bool commit(uint32_t commit_idx) = 0;
    // Reset application state, so that any requests following and including abort_idx are rolled back
    virtual bool abort(uint32_t abort_idx) = 0;

    virtual bool applySnapshot(const std::string &snapshot, const std::string &digest, uint32_t idx) = 0;
    virtual AppSnapshot getLatestSnapshot() = 0;
};

class ApplicationClient {
public:
    virtual ~ApplicationClient() = default;

    // Generate app traffic as serialized message
    virtual std::string generateAppRequest() = 0;
};

#endif