#include "kv_store.h"

#include "zipfian.h"

#include <random>
#include <sstream>

using namespace dombft::apps;

KVStore::KVStore(uint32_t numKeys)
    : committedIdx(0)
{
    int width = std::to_string(numKeys - 1).length();
    for (uint32_t i = 0; i < numKeys; i++) {
        data[std::to_string(i)] = "";
        committedData[std::to_string(i)] = "";
    }
}

KVStore::~KVStore() { snapshotThread_.join(); }

std::string KVStore::execute(const std::string &serialized_request, uint32_t execute_idx)
{
    KVRequest req;
    if (!req.ParseFromString(serialized_request)) {
        LOG(ERROR) << "Failed to parse KVRequest";
        return "";
    }
    KVResponse response;

    KVRequestType type = req.msg_type();
    std::string key = req.key();
    std::string value = req.value();

    if (type == KVRequestType::GET) {
        if (data.count(key)) {
            response.set_ok(true);
            response.set_value(data[key]);
            VLOG(6) << "GET key: " << key << " value: " << data[key];
        } else {
            response.set_ok(false);
        }
    } else if (type == KVRequestType::SET) {
        data[key] = req.value();
        response.set_ok(true);
        VLOG(6) << "SET key: " << key << " value: " << req.value();

    } else if (type == KVRequestType::DELETE) {
        bool deleted = data.erase(key);
        response.set_ok(deleted);
        if (deleted) {
            VLOG(6) << "DELETE key: " << key << " success";
        } else {
            VLOG(6) << "DELETE key: " << key << " fail";
        }
    } else {
        LOG(ERROR) << "Unknown KVRequestType";
        return "";
    }

    std::string ret;
    if (!response.SerializeToString(&ret)) {
        throw std::runtime_error("Failed to serialize CounterResponse message.");
    }
    requests.push_back({execute_idx, key, value, type});

    return ret;
}

bool KVStore::commit(uint32_t idx)
{
    LOG(INFO) << "Committing kv store at idx: " << idx;

    uint32_t i = 0;

    {
        LOG(INFO) << "Getting lock on committedData: " << idx;

        std::unique_lock<std::shared_mutex> lock(committedDataMutex_);

        VLOG(6) << "Copying requests into committedData";
        for (i = 0; i < requests.size() && requests[i].idx <= idx; i++) {
            KVStoreRequest &r = requests[i];
            if (r.type == KVRequestType::SET) {
                committedData[r.key] = r.value;
            } else if (r.type == KVRequestType::DELETE) {
                committedData.erase(r.key);
            }
        }
        VLOG(6) << "Done copying requests into committedData";

        committedIdx = idx;
    }

    // remove committed requests
    requests.erase(requests.begin(), requests.begin() + i);

    LOG(INFO) << "Committed at idx: " << idx << " committed_data size: " << committedData.size()
              << " requests size: " << requests.size();

    if (snapshotThread_.joinable()) {
        snapshotThread_.join();
    }

    // TODO: minor race condition here if commit again before the snapshot thread is able to acquire lock
    // Won't worry about this for now...
    snapshotThread_ = std::thread([this, idx]() {
        AppSnapshot ret;
        VLOG(6) << "Starting snapshot of KVStore";

        uint64_t now = GetMicrosecondTimestamp();

        uint32_t idx = committedIdx;
        std::string snapshot;
        std::string digest;

        // TODO this only works if key/value data does not have ":" or ","
        // we should use a better serialization format
        {
            std::shared_lock<std::shared_mutex> lock(committedDataMutex_);
            for (auto &kv : committedData) {
                snapshot += kv.first + ":" + kv.second + ",";
            }
        }
        VLOG(6) << "Snapshot of KVStore done copying committed data";

        byte digestBytes[SHA256_DIGEST_LENGTH];
        SHA256_CTX ctx;
        SHA256_Init(&ctx);
        SHA256_Update(&ctx, snapshot.c_str(), snapshot.size());
        SHA256_Final(digestBytes, &ctx);

        digest = std::string(digestBytes, digestBytes + SHA256_DIGEST_LENGTH);

        {
            std::lock_guard<std::mutex> lock(snapshotMutex_);

            snapshot_.snapshot = std::make_shared<std::string>(snapshot);
            snapshot_.digest = digest;
            snapshot_.seq = idx;
        }

        VLOG(6) << "Snapshot of KVStore took " << (GetMicrosecondTimestamp() - now) / 1000 << " ms";
    });

    return true;
}

bool KVStore::abort(uint32_t abort_idx)
{

    LOG(INFO) << "Aborting operations starting from idx=" << abort_idx;
    if (abort_idx <= committedIdx) {
        LOG(WARNING) << "Abort index is less than committed request with index " << requests.front().idx
                     << ", abort failed";
        return false;
    }
    if (requests.empty() || abort_idx > requests.back().idx) {
        LOG(WARNING) << "Abort index is greater than the newest uncommitted request with index " << requests.back().idx
                     << ". Nothing will happen.";
        return true;
    }

    {
        std::shared_lock<std::shared_mutex> lock(committedDataMutex_);

        // reapply committed data and ops before abort_idx
        data = committedData;
    }

    uint32_t i = 0;
    for (auto &r : requests) {
        if (r.idx >= abort_idx) {
            requests.erase(requests.begin() + i, requests.end());
            break;
        }

        if (r.type == KVRequestType::SET) {
            data[r.key] = r.value;
        } else if (r.type == KVRequestType::DELETE) {
            data.erase(r.key);
        }
        i++;
    }
    return true;
}

bool KVStore::applySnapshot(const std::string &snapshot, const std::string &digest, uint32_t idx)
{
    // Acquire both locks here
    // TODO, could maybe be a bit more fine grained here
    std::scoped_lock lock(committedDataMutex_, snapshotMutex_);

    byte computedDigest[SHA256_DIGEST_LENGTH];
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, snapshot.c_str(), snapshot.size());
    SHA256_Final(computedDigest, &ctx);

    VLOG(4) << digest.size() << " " << digest_to_hex(digest);
    VLOG(4) << digest_to_hex(std::string(computedDigest, computedDigest + SHA256_DIGEST_LENGTH));

    if (std::string(computedDigest, computedDigest + SHA256_DIGEST_LENGTH) != digest) {
        LOG(ERROR) << "Snapshot digest does not match";
        return false;
    }

    try {
        std::unordered_map<std::string, std::string> newData;

        std::istringstream iss(snapshot);
        std::string kv;
        while (std::getline(iss, kv, ',')) {
            std::istringstream kvss(kv);
            std::string key, value;
            std::getline(kvss, key, ':');
            std::getline(kvss, value, ':');
            newData[key] = value;
        }

        LOG(INFO) << "Applied snapshot, data size: " << data.size();

        std::swap(data, newData);
        committedData = data;
        committedIdx = idx;
        snapshot_ = {idx, std::make_shared<std::string>(snapshot), digest};

    } catch (std::exception &e) {
        LOG(ERROR) << "Failed to parse snapshot: " << e.what();
        return false;
    }

    return true;
}

AppSnapshot KVStore::getSnapshot()
{
    std::lock_guard<std::mutex> lock(snapshotMutex_);

    // TODO copying this is not ideal since it is pretty large.
    AppSnapshot ret = snapshot_;

    return ret;
}

KVStoreClient::KVStoreClient(uint32_t numKeys)
    : keyDist_(numKeys)
{
    LOG(INFO) << "KVStore numKeys=" << numKeys << "\tskewFactor=" << SKEW_FACTOR;
    std::default_random_engine generator(GetMicrosecondTimestamp());
    zipfian_int_distribution<uint32_t> zipfianDistribution(0, numKeys, SKEW_FACTOR);
    for (uint32_t i = 0; i < keyDist_.size(); i++) {
        keyDist_[i] = zipfianDistribution(generator);
    }
}

std::string KVStoreClient::generateAppRequest()
{
    static uint64_t num = 0;

    // TODO only create writes for now, if we implement some sort of read optimization,
    // we can change this.
    KVRequest req;
    req.set_key(std::to_string(keyDist_[num % keyDist_.size()]));
    req.set_value(std::to_string(num));
    num++;

    req.set_msg_type(KVRequestType::SET);
    VLOG(6) << "Generated request: " << req.key() << " " << req.value();
    return req.SerializeAsString();
}
