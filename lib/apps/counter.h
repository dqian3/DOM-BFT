#ifndef COUNTER_H
#define COUNTER_H

#include "lib/application.h"

#include <unordered_map>
#include <string>
#include <glog/logging.h>

#define INT_SIZE_IN_BYTES (sizeof(int))

// TODO instead of requests and responses being raw bytes, have 
// request and response types that can be serialized/unserialized.
class Counter : public Application {
public:
    int counter;

    int counter_stable;

    byte commit_digest[INT_SIZE_IN_BYTES];
    byte snapshot_digest[INT_SIZE_IN_BYTES];



    virtual ~Counter();

    virtual std::unique_ptr<AppLayerResponse> execute(const std::string &serialized_request) override;

    virtual bool commit(uint32_t commit_idx, byte* committed_value) override;

    virtual byte* getDigest(uint32_t digest_idx) override;

    virtual byte* takeSnapshot() override;

    Counter() : counter(0), counter_stable(0) {}

    virtual bool abort() override;
    
};

class CounterTrafficGen : public AppTrafficGen {
public:
    CounterTrafficGen() = default;

    void* generateAppTraffic() override;
};

#endif