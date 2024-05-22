#ifndef KV_STORE_H
#define KV_STORE_H

#include "lib/application.h"

#include <unordered_map>
#include <string>

// TODO instead of requests and responses being raw bytes, have 
// request and response types that can be serialized/unserialized.
class KVStore : public Application {
    std::unordered_map<std::string, std::string> data;

public:
    virtual ~KVStore();

    virtual std::unique_ptr<AppResponse> execute(const AppRequest &request) override;
};

#endif