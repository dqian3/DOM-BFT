#include "kv_store.h"

#include "proto/dombft_apps.pb.h"

using namespace dombft::apps;

KVStore::~KVStore() {}

std::string KVStore::execute(const std::string &serialized_request, const uint32_t execute_idx)
{
    std::unique_ptr<KVRequest> kvReq = std::make_unique<KVRequest>();
    if (!kvReq->ParseFromString(serialized_request)) {
        LOG(ERROR) << "Failed to parse KVRequest";
        return nullptr;
    }
    KVResponse response;

    std::string key = kvReq->key();

    if (kvReq->msg_type() == KVRequestType::GET) {
        if (data.count(key)) {
            response.set_ok(true);
            response.set_value(data[key]);
        } else {
            response.set_ok(false);
        }

    } else if (kvReq->msg_type() == KVRequestType::SET) {
        data[key] = kvReq->value();   // TODO check value is there
        response.set_ok(true);
    } else if (kvReq->msg_type() == KVRequestType::DELETE) {
        if (data.count(key)) {
            data.erase(key);
            response.set_ok(true);
        } else {
            response.set_ok(false);
        }
    } else {
        return "";
    }

    std::string ret;
    if (!response.SerializeToString(&ret)) {
        LOG(ERROR) << "Failed to serialize CounterResponse";
        throw std::runtime_error("Failed to serialize CounterResponse message.");
        return "";
    }

    return ret;
}