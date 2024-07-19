#ifndef COUNTER_H
#define COUNTER_H

#include "lib/application.h"
#include "lib/log.h"


#include <unordered_map>
#include <string>

// TODO instead of requests and responses being raw bytes, have 
// request and response types that can be serialized/unserialized.
class Counter : public Application {
    int counter;

    int counter_stable;

    std::shared_ptr<Log> log_;

public:
    virtual ~Counter();

    virtual std::unique_ptr<AppResponse> execute(const AppRequest &request) override;

    virtual bool commit(uint32_t commit_idx) override;

    virtual byte* getDigest(uint32_t digest_idx) override;

    virtual byte* takeSnapshot() override;
};

#endif