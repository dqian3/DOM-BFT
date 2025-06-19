#ifndef KV_STORE_H
#define KV_STORE_H

#include "lib/application.h"
#include "lib/utils.h"
#include "proto/dombft_apps.pb.h"

#include <iomanip>
#include <optional>
#include <string>
#include <unordered_map>

#include <mutex>
#include <shared_mutex>

// THese should prboably be in a config file, but they don't actually affect our protocol
// so I was lazy and put them here...
#define NUM_KEYS    500000
#define SKEW_FACTOR 0.9

struct KVStoreRequest {
    uint32_t idx;
    std::string key;
    std::string value;
    dombft::apps::KVRequestType type;
};

class KVStore : public Application {

private:
    std::vector<KVStoreRequest> requests;
    std::unordered_map<std::string, std::string> data;
    std::unordered_map<std::string, std::string> committedData;
    uint32_t dataIdx;
    uint32_t committedIdx;

    std::shared_mutex committedDataMutex_;

    std::thread snapshotThread_;

public:
    KVStore(uint32_t numKeys = NUM_KEYS);

    ~KVStore() override;

    std::string execute(const std::string &serialized_request, uint32_t execute_idx) override;

    bool commit(uint32_t idx) override;
    bool abort(uint32_t idx) override;

    void takeSnapshot(SnapshotCallback cb) override;

    bool applySnapshot(const std::string &snapshot, const std::string &digest, uint32_t idx) override;
};

class KVStoreClient : public ApplicationClient {
    std::vector<uint32_t> keyDist_;

public:
    KVStoreClient(uint32_t numKeys = NUM_KEYS);

    std::string generateAppRequest() override;
};

#endif