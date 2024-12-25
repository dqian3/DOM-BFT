#include "kv_store.h"

KVStore::~KVStore() {}

std::string KVStore::execute(const std::string &serialized_request, const uint32_t execute_idx)
{
    std::unique_ptr<KVRequest> kvReq = std::make_unique<KVRequest>();
    if (!kvReq->ParseFromString(serialized_request)) {
        LOG(ERROR) << "Failed to parse KVRequest";
        return {};
    }
    KVResponse response;

    std::string key = kvReq->key();
    std::string value = {};
    KVRequestType type = kvReq->msg_type();
    if (type == KVRequestType::GET) {
        if (data.count(key)) {
            response.set_ok(true);
            response.set_value(data[key]);
            VLOG(6) << "GET key: " << key << " value: " << data[key];
        } else {
            response.set_ok(false);
        }
    } else if (type == KVRequestType::SET) {
        data[key] = kvReq->value();   // TODO check value is there   <--why
        response.set_ok(true);
        VLOG(6) << "SET key: " << key << " value: " << kvReq->value();
    } else if (type == KVRequestType::DELETE) {
        if (data.count(key)) {
            data.erase(key);
            response.set_ok(true);
            VLOG(6) << "DELETE key: " << key;
        } else {
            response.set_ok(false);
        }
    } else {
        LOG(ERROR) << "Unknown KVRequestType";
        return "";
    }

    std::string ret;
    if (!response.SerializeToString(&ret)) {
        LOG(ERROR) << "Failed to serialize CounterResponse";
        throw std::runtime_error("Failed to serialize CounterResponse message.");
    }
    requests.push_back({execute_idx,key,value,type});

    return ret;
}

std::string KVStore::getDigest(uint32_t digest_idx)
{
    if (digest_idx >= requests.back().idx) {
        LOG(ERROR)<< "Invalid digest idx: " << digest_idx << " out of range";
        return {};
    }
    byte digest[SHA256_DIGEST_LENGTH];
    std::string all;
    // we use request history as part of the digest instead of data
    // since it is easier to keep track of the idx
    for (auto &ele : requests) {
        all += std::to_string(ele.idx) + ele.key + ele.value + std::to_string(static_cast<int>(ele.type));
        if (ele.idx == digest_idx) {
            break;
        }
    }
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, committed_data_digest, SHA256_DIGEST_LENGTH);
    SHA256_Update(&ctx, all.c_str(), all.size());
    SHA256_Final(digest, &ctx);
    return {reinterpret_cast<char *>(digest), SHA256_DIGEST_LENGTH};
}

bool KVStore::takeSnapshot()
{
    std::string sp = {};
    for (auto &kv : data) {
        sp += kv.first + ":" + kv.second + ",";
    }
    snapshots_data[requests.back().idx] = sp;
    return true;
}

std::string KVStore::getSnapshot(uint32_t seq)
{
    return snapshots_data.count(seq) ? snapshots_data[seq] : "";
}

void KVStore::applySnapshot(const std::string &snapshot)
{
    try{
        std::istringstream iss(snapshot);
        std::string kv;
        data.clear();
        while (std::getline(iss, kv, ',')) {
            std::istringstream kvss(kv);
            std::string key, value;
            std::getline(kvss, key, ':');
            std::getline(kvss, value, ':');
            data[key] = value;
        }
        LOG(INFO) << "Applied snapshot, data size: " << data.size();
    }catch(int e){
        LOG(ERROR) << "Failed to apply snapshot";
    }
}

bool KVStore::abort(const uint32_t abort_idx)
{
    LOG(INFO) << "Aborting operations after idx: " << abort_idx;
    if (abort_idx < requests.front().idx || abort_idx > requests.back().idx) {
        // or revert to committed state?
        LOG(ERROR) << "Abort index is either less than the oldest uncommitted request with index " << requests.front().idx
                      << " or greater than the newest uncommitted request with index " << requests.back().idx
                   << ". Unable to revert.";
        return false;
    }
    // reapply committed data and ops before abort_idx
    data = committed_data;
    for (auto &ele : requests) {
        if (ele.idx > abort_idx) {
            break;
        }
        if (ele.type == KVRequestType::SET) {
            data[ele.key] = ele.value;
        } else if (ele.type == KVRequestType::DELETE) {
            data.erase(ele.key);
        }
    }
    return true;
}

bool KVStore::commit(uint32_t commit_idx) {
    LOG(INFO) << "Committing counter value at idx: " <<commit_idx;
    // update committed_data_digest alone the way
    byte digest[SHA256_DIGEST_LENGTH];
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    std::string all;

    uint32_t i = 0;
    for (auto &ele : requests) {
        if (ele.idx > commit_idx) {
            break;
        }
        if (ele.type == KVRequestType::SET) {
            committed_data[ele.key] = ele.value;
        } else if (ele.type == KVRequestType::DELETE) {
            committed_data.erase(ele.key);
        }
        i++;
        all += std::to_string(ele.idx) + ele.key + ele.value + std::to_string(static_cast<int>(ele.type));
    }
    // remove committed requests
    requests.erase(requests.begin(), requests.begin() + i);

    SHA256_Update(&ctx, committed_data_digest, SHA256_DIGEST_LENGTH);
    SHA256_Update(&ctx, all.c_str(), all.size());
    SHA256_Final(digest, &ctx);
    memcpy(committed_data_digest, digest, SHA256_DIGEST_LENGTH);
    LOG(INFO) << "Committed at idx: " << commit_idx << " committed_data size: " << committed_data.size()
            << " digest: " << std::string(reinterpret_cast<char *>(committed_data_digest), SHA256_DIGEST_LENGTH);

    // remove snapshots that are older than commit_idx
    auto it = snapshots_data.begin();
    while (it != snapshots_data.end()) {
        if (it->first < commit_idx) {
            it = snapshots_data.erase(it);
        } else {
            ++it;
        }
    }
    return true;
}

void KVStore::storeAppStateInYAML()
{
    // TODO(Hao): maybe add some metadata as well
    std::ofstream fout(APP_STATE_YAML_FILE);
    YAML::Node node;
    for (auto &kv : data) {
        node[kv.first] = kv.second;
    }
    fout << node;
    std::cout << "App state saved to " << APP_STATE_YAML_FILE << std::endl;
}

