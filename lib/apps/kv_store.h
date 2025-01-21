#ifndef KV_STORE_H
#define KV_STORE_H

#include "lib/application.h"
#include "proto/dombft_apps.pb.h"
#include "lib/utils.h"
#include <string>
#include <unordered_map>

using namespace dombft::apps;

#define KEY_MAX_LENGTH 6
#define KEY_MIN_LENGTH 2
#define VALUE_MAX_LENGTH 6
#define VALUE_MIN_LENGTH 2

typedef struct {
    uint32_t idx;
    std::string key;
    std::string value;
    KVRequestType type;
} KVStoreRequest;

// TODO instead of requests and responses being raw bytes, have
// request and response types that can be serialized/unserialized.
class KVStore : public Application {
    // we still need this since abort needs it. REMOVE cmt after abort is implemented!
    std::vector<KVStoreRequest> requests;
    std::map<std::string, std::string> data;
    std::map<std::string, std::string> committed_data;
    byte committed_data_digest[SHA256_DIGEST_LENGTH];
    // seq -> data
    std::map<uint32_t, std::string> snapshots_data;
public:
    ~KVStore();

    std::string execute(const std::string &serialized_request, const uint32_t execute_idx) override;

    bool commit(uint32_t commit_idx) override;

    std::string getDigest(uint32_t digest_idx) override;

    bool takeSnapshot() override;

    std::string getSnapshot(uint32_t seq) override;

    void applySnapshot(const std::string &snapshot) override;

    bool abort(const uint32_t abort_idx) override;

    void storeAppStateInYAML(const std::string& filename) override;

};

class KVStoreTrafficGen : public AppTrafficGen {
    uint32_t keyLen;
    uint32_t valLen;
public:
    // TODO(Hao): perhaps make length configurable
    KVStoreTrafficGen() : keyLen(KEY_MIN_LENGTH), valLen(VALUE_MIN_LENGTH) {}

    std::string randomStringNormDist(std::string::size_type length);
    void *generateAppTraffic() override;
};

#endif