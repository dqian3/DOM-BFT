#ifndef KV_STORE_H
#define KV_STORE_H

#include "lib/application.h"
#include "lib/utils.h"
#include "proto/dombft_apps.pb.h"

#include <iomanip>
#include <optional>
#include <string>
#include <unordered_map>

#define KEY_MAX_LENGTH   6
#define KEY_MIN_LENGTH   2
#define VALUE_MAX_LENGTH 6
#define VALUE_MIN_LENGTH 2

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
    uint32_t committedIdx;

public:
    KVStore(uint32_t numKeys)
        : committedIdx(0)
    {
        int width = std::to_string(numKeys - 1).length();
        for (uint32_t i = 0; i < numKeys; i++) {
            std::stringstream ss;
            ss << std::setfill('0') << std::setw(width) << i;
            data[ss.str()] = "";
        }
    }

    std::string execute(const std::string &serialized_request, uint32_t execute_idx) override;

    bool commit(uint32_t commit_idx) override;
    bool abort(uint32_t abort_idx) override;

    bool applySnapshot(const std::string &snapshot, const std::string &digest) override;

    AppSnapshot takeSnapshot() override;
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

    std::string randomString(std::string::size_type length);
    std::string generateAppRequest() override;
};

#endif