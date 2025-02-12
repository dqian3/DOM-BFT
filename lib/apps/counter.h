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

    std::string serialize() const
    {
        std::string serialized;
        serialized.append((char *) &version, sizeof(version));
        serialized.append((char *) &value, sizeof(value));
        return serialized;
    }
} VersionedValue;

class Counter : public Application {
public:
    int counter;
    VersionedValue committed_state;

    Counter()
        : counter(0)
        , committed_state(0, 0)
        , version_hist()
    {
    }

    ~Counter() override;

    std::string execute(const std::string &serialized_request, const uint32_t execute_idx) override;

    bool commit(uint32_t commit_idx) override;

    std::string getDigest(uint32_t digest_idx) override;

    // counter just uses digest as snapshot that is returned directly by getSnapshot
    inline bool takeDelta() override { return true; }
    inline bool takeSnapshot() override { return true; }

    inline std::string getDelta(uint32_t seq) override { return getDigest(seq); };
    inline std::shared_ptr<std::string> getSnapshot(uint32_t seq) override;

    void applyDelta(const std::string &delta) override;
    void applySnapshot(const std::string &snapshot) override;

    void storeAppStateInYAML(const std::string &filename) override;

    bool abort(const uint32_t abort_idx) override;

private:
    std::vector<VersionedValue> version_hist;
};

class CounterClient : public ApplicationClient {
public:
    CounterClient() = default;

    std::string generateAppRequest() override;
};

#endif