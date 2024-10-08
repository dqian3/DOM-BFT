#ifndef COUNTER_H
#define COUNTER_H

#include "lib/application.h"

#include <glog/logging.h>
#include <string>
#include <unordered_map>

#include <vector>

#define INT_SIZE_IN_BYTES (sizeof(int))

typedef struct VersionedValue {
    uint64_t version;
    int64_t value;
} VersionedValue;

class Counter : public Application {
public:
    int counter;

    VersionedValue committed_state;

    virtual ~Counter();

    virtual std::string execute(const std::string &serialized_request, const uint32_t execute_idx) override;

    virtual bool commit(uint32_t commit_idx) override;

    virtual std::string getDigest(uint32_t digest_idx) override;

    virtual std::string takeSnapshot() override;

    virtual void applySnapshot(const std::string &snapshot) override;

    Counter()
        : counter(0)
        , committed_state(0, 0)
        , version_hist()
    {
    }

    virtual bool abort(const uint32_t abort_idx) override;

private:
    std::vector<VersionedValue> version_hist;
};

class CounterTrafficGen : public AppTrafficGen {
public:
    CounterTrafficGen() = default;

    void *generateAppTraffic() override;
};

#endif