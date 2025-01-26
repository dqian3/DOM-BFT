#include "kv_store.h"

#include <random>

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
    // print out the request
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
        bool deleted = data.erase(key);
        response.set_ok(deleted);
        if (deleted)
            VLOG(6) << "DELETE key: " << key;
    } else {
        LOG(ERROR) << "Unknown KVRequestType";
        return "";
    }

    std::string ret;
    if (!response.SerializeToString(&ret)) {
        LOG(ERROR) << "Failed to serialize CounterResponse";
        throw std::runtime_error("Failed to serialize CounterResponse message.");
    }
    requests.push_back({execute_idx, key, value, type});
    return ret;
}

std::string KVStore::getDigest(uint32_t digest_idx)
{
    if (digest_idx >= requests.back().idx) {
        LOG(ERROR) << "Invalid digest idx: " << digest_idx << " out of range";
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

bool KVStore::takeDelta()
{
    if (requests.empty())
        return false;

    std::string sp = {};
    if (committed_data.empty()) {
        for (auto &kv : data) {
            sp += kv.first + ":" + kv.second + ",";
        }
    } else {
        std::map<std::string, std::string> delta = getDeltaFromCommit();
        for (auto &kv : delta) {
            sp += kv.first + ":" + kv.second + ",";
        }
    }
    delta_data[requests.back().idx] = sp;
    return true;
}

bool KVStore::takeSnapshot()
{
    std::string sp = {};
    for (auto &kv : data) {
        sp += kv.first + ":" + kv.second + ",";
    }
    snapshots_data[requests.back().idx] = std::make_shared<std::string>(sp);
    return true;
}

std::shared_ptr<std::string> KVStore::getSnapshot(uint32_t seq) { return snapshots_data.count(seq) ? snapshots_data[seq] : nullptr; }
std::string KVStore::getDelta(uint32_t seq) { return delta_data.count(seq) ? delta_data[seq] : ""; }

void KVStore::applyDelta(const std::string &delta)
{
    try {
        std::istringstream iss(delta);
        std::string kv;
        data = committed_data;
        while (std::getline(iss, kv, ',')) {
            std::istringstream kvss(kv);
            std::string key, value;
            std::getline(kvss, key, ':');
            std::getline(kvss, value, ':');
            if (value.empty()) {
                data.erase(key);
            } else {
                data[key] = value;
            }
        }
        LOG(INFO) << "Applied delta, data size: " << data.size();
    } catch (std::exception &e) {
        LOG(ERROR) << "Failed to apply delta: " << e.what();
    }
}
void KVStore::applySnapshot(const std::string &snapshot)
{
    try {
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
    } catch (std::exception &e) {
        LOG(ERROR) << "Failed to apply snapshot: " << e.what();
    }
}

std::map<std::string, std::string> KVStore::getDeltaFromCommit()
{
    std::map<std::string, std::string> delta;
    for (const auto &req : requests) {
        if (req.type == KVRequestType::SET) {
            delta[req.key] = req.value;
        } else if (req.type == KVRequestType::DELETE) {
            // empty string means delete
            delta[req.key] = "";
        }
    }
    return delta;
}

bool KVStore::abort(const uint32_t abort_idx)
{
    LOG(INFO) << "Aborting operations after idx: " << abort_idx;
    if (abort_idx < requests.front().idx) {
        LOG(ERROR) << "Abort index is  less than the oldest uncommitted request with index " << requests.front().idx
                   << ". Unable to revert.";
        return false;
    }
    if (abort_idx > requests.back().idx) {
        LOG(ERROR) << "Abort index is greater than the newest uncommitted request with index " << requests.back().idx
                   << ". Unable to revert.";
        return false;
    }
    // reapply committed data and ops before abort_idx
    data = committed_data;
    uint32_t i = 0;
    for (auto &ele : requests) {
        if (ele.idx > abort_idx) {
            requests.erase(requests.begin() + i, requests.end());
            break;
        }
        // TODO(Hao): can be optimized by using a set to keep track of keys as later ops can override earlier ops
        if (ele.type == KVRequestType::SET) {
            data[ele.key] = ele.value;
        } else if (ele.type == KVRequestType::DELETE) {
            data.erase(ele.key);
        }
        i++;
    }
    return true;
}

bool KVStore::commit(uint32_t commit_idx)
{
    LOG(INFO) << "Committing counter value at idx: " << commit_idx;
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
        // TODO(Hao): can be optimized by using a set to keep track of keys as later ops can override earlier ops
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
              << " digest: " << digest_to_hex(committed_data_digest, SHA256_DIGEST_LENGTH);

    // remove snapshots and delta that are older than commit_idx
    auto it = snapshots_data.begin();
    while (it != snapshots_data.end()) {
        if (it->first < commit_idx) {
            delta_data.erase(it->first);
            it = snapshots_data.erase(it);
        } else {
            ++it;
        }
    }
    return true;
}

void KVStore::storeAppStateInYAML(const std::string &filename)
{
    // TODO(Hao): maybe add some metadata as well
    std::ofstream fout(filename);
    YAML::Node node;
    for (auto &kv : data) {
        node[kv.first] = kv.second;
    }
    fout << node;
    std::cout << "App state saved to " << filename << std::endl;
}

std::string KVStoreTrafficGen::randomStringNormDist(std::string::size_type length)
{
    static auto &chrs = "0123456789"
                        "abcdefghijklmnopqrstuvwxyz"
                        "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    thread_local static std::mt19937 rg{std::random_device{}()};
    thread_local static std::uniform_int_distribution<std::string::size_type> pick(0, sizeof(chrs) - 2);
    std::string s;
    s.reserve(length);
    while (length--)
        s += chrs[pick(rg)];
    return s;
}
void *KVStoreTrafficGen::generateAppTraffic()
{
    // TODO(Hao): test with set only for now.
    KVRequest *req = new KVRequest();
    req->set_key(randomStringNormDist(keyLen));
    req->set_value(randomStringNormDist(valLen));
    req->set_msg_type(KVRequestType::SET);
    // TODO(Hao): make it more random, now KV always have same length
    keyLen = (keyLen + 1) > KEY_MAX_LENGTH ? KEY_MIN_LENGTH : (keyLen + 1);
    valLen = (valLen + 1) > VALUE_MAX_LENGTH ? VALUE_MIN_LENGTH : (valLen + 1);
    LOG(INFO) << "Generated request: " << req->key() << " " << req->value();
    return req;
}
