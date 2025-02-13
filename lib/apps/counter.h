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
    int value;

} VersionedValue;

class Counter : public Application {
public:
    Counter()
        : counter(0)
        , committed_state(0, 0)
        , version_hist()
    {
    }

    ~Counter() override;

    std::string execute(const std::string &serialized_request, const uint32_t execute_idx) override;

    bool commit(uint32_t commit_idx) override;
    bool abort(const uint32_t abort_idx) override;

    bool applyDelta(const std::string &delta, const std::string &digest) override;
    bool applySnapshot(const std::string &snapshot, const std::string &digest) override;

    AppSnapshot takeSnapshot() override;

private:
    int counter;
    VersionedValue committed_state;

    std::vector<VersionedValue> version_hist;
};

class CounterClient : public ApplicationClient {
public:
    CounterClient() = default;

    std::string generateAppRequest() override;
};

#endif