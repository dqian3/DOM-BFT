#ifndef COUNTER_H
#define COUNTER_H

#include "lib/application.h"

#include <glog/logging.h>
#include <string>
#include <unordered_map>

#include <vector>

class Counter : public Application {
public:
    Counter()
        : counter(0)
        , committedValue(0)
        , committedIdx(0)
    {
    }

    ~Counter() override;

    std::string execute(const std::string &serialized_request, uint32_t execute_idx) override;

    bool commit(uint32_t commit_idx) override;
    bool abort(uint32_t abort_idx) override;

    bool applySnapshot(const std::string &snapshot, const std::string &digest, uint32_t idx) override;

    ::AppSnapshot getLatestSnapshot() override;

private:
    int counter;

    std::map<uint64_t, uint64_t> values;

    int committedValue;
    int committedIdx;
};

class CounterClient : public ApplicationClient {
public:
    CounterClient() = default;

    std::string generateAppRequest() override;
};

#endif