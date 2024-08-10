#include "kv_store.h"

#include "proto/dombft_apps.pb.h"

using namespace dombft::apps;

KVStore::~KVStore() {}

std::unique_ptr<AppLayerResponse> KVStore::execute(const std::string &serialized_request)
{
    std::unique_ptr<KVRequest> kvReq = std::make_unique<KVRequest>();
    if (!kvReq->ParseFromString(serialized_request)) {
        LOG(ERROR) << "Failed to parse KVRequest";
        return nullptr;
    }
    std::unique_ptr<KVResponse> ret = std::make_unique<KVResponse>();

    std::string key = kvReq->key();

    if (kvReq->msg_type() == KVRequestType::GET) {
        if (data.count(key)) {
            ret->set_ok(true);
            ret->set_value(data[key]);
        } else {
            ret->set_ok(false);
        }

    } else if (kvReq->msg_type() == KVRequestType::SET) {
        data[key] = kvReq->value();   // TODO check value is there
        ret->set_ok(true);
    } else if (kvReq->msg_type() == KVRequestType::DELETE) {
        if (data.count(key)) {
            data.erase(key);
            ret->set_ok(true);
        } else {
            ret->set_ok(false);
        }
    } else {
        return nullptr;
    }

    return NULL;
}
