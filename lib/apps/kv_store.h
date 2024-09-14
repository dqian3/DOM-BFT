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
    virtual ~KVStore();

    virtual std::string execute(const std::string &serialized_request, const uint32_t execute_idx) override;

    virtual bool commit(uint32_t commit_idx) override { return true; }

    virtual std::string getDigest(uint32_t digest_idx) override;

    virtual std::string takeSnapshot() override;

    virtual void applySnapshot(const std::string &snapshot) override;

    virtual bool abort(const uint32_t abort_idx) override;
};

#endif