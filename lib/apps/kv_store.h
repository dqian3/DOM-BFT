#ifndef KV_STORE_H
#define KV_STORE_H

#include "lib/application.h"
#include "lib/utils.h"
#include "proto/dombft_apps.pb.h"
#include <string>
#include <unordered_map>

using namespace dombft::apps;

#define KEY_MAX_LENGTH   6
#define KEY_MIN_LENGTH   2
#define VALUE_MAX_LENGTH 6
#define VALUE_MIN_LENGTH 2

typedef struct {
    uint32_t idx;
    std::string key;
    std::string value;
    KVRequestType type;
} KVStoreRequest;

class KVStore : public Application {
    std::vector<KVStoreRequest> requests;
    std::map<std::string, std::string> data;
    std::map<std::string, std::string> committed_data;
    byte committed_data_digest[SHA256_DIGEST_LENGTH];
    // seq -> data
    std::map<uint32_t, std::shared_ptr<std::string>> snapshots_data;
    std::map<uint32_t, std::string> delta_data;

public:
    ~KVStore();

    std::string execute(const std::string &serialized_request, const uint32_t execute_idx) override;

    bool commit(uint32_t commit_idx) override;

    std::string getDigest(uint32_t digest_idx) override;

    bool takeSnapshot() override;
    bool takeDelta() override;

    std::shared_ptr<std::string> getSnapshot(uint32_t seq) override;
    std::string getDelta(uint32_t seq) override;

    void applySnapshot(const std::string &snapshot) override;
    void applyDelta(const std::string &delta) override;

    bool abort(const uint32_t abort_idx) override;

    std::map<std::string, std::string> getDeltaFromCommit();

    void storeAppStateInYAML(const std::string &filename) override;
};

class KVStoreClient : public ApplicationClient {
    uint32_t keyLen;
    uint32_t valLen;

public:
    KVStoreClient()
        : keyLen(KEY_MIN_LENGTH)
        , valLen(VALUE_MIN_LENGTH)
    {
    }

    std::string randomStringNormDist(std::string::size_type length);
    std::string generateAppRequest() override;
};

#endif