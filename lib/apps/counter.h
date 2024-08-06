#ifndef COUNTER_H
#define COUNTER_H

#include "lib/application.h"

#include <unordered_map>
#include <string>
#include <glog/logging.h>

#include <vector>

#define INT_SIZE_IN_BYTES (sizeof(int))


typedef struct VersionedValue {
    uint64_t version;
    int value;
} VersionedValue;

// TODO instead of requests and responses being raw bytes, have 
// request and response types that can be serialized/unserialized.
class Counter : public Application {
public:
    int counter;

    int counter_stable;

    virtual ~Counter();

    virtual std::string execute(const std::string &serialized_request, const uint64_t timestamp) override;

    virtual bool commit(uint32_t commit_idx) override;

    virtual std::unique_ptr<byte[]> getDigest(uint32_t digest_idx) override;

    virtual std::unique_ptr<byte[]> takeSnapshot() override;

    Counter() : counter(0), counter_stable(0), version_hist() {}

    virtual bool abort() override;

private:
    std::vector<VersionedValue> version_hist;

    uint64_t committed_idx = 0;
    
};

class CounterTrafficGen : public AppTrafficGen {
public:
    CounterTrafficGen() = default;

    void* generateAppTraffic() override;
};

#endif