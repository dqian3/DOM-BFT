#ifndef KV_STORE_H
#define KV_STORE_H

#include "lib/application.h"

#include <string>
#include <unordered_map>

// TODO instead of requests and responses being raw bytes, have
// request and response types that can be serialized/unserialized.
class KVStore : public Application {
    std::unordered_map<std::string, std::string> data;

public:
    ~KVStore();

    std::string execute(const std::string &serialized_request, const uint32_t execute_idx) override;

    bool commit(uint32_t commit_idx) override { return true; }

    std::string getDigest(uint32_t digest_idx) override;

    std::string takeSnapshot() override;

    void applySnapshot(const std::string &snapshot) override;

    bool abort(const uint32_t abort_idx) override;
};

#endif