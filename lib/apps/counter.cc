#include "counter.h"

#include "proto/dombft_apps.pb.h"

using namespace dombft::apps;

Counter::~Counter() {}

std::string Counter::execute(const std::string &serialized_request, const uint32_t execute_idx)
{
    std::unique_ptr<CounterRequest> counterReq = std::make_unique<CounterRequest>();
    if (!counterReq->ParseFromString(serialized_request)) {
        LOG(ERROR) << "Failed to parse CounterRequest";
        return "";
    }

    CounterOperation op = counterReq->op();
    CounterResponse response;

    if (op == CounterOperation::INCREMENT) {
        VLOG(6) << "Incrementing counter";
        counter++;
        VLOG(6) << "Counter value: " << counter;
    } else if (op == CounterOperation::DECREMENT) {
        VLOG(6) << "Decrementing counter";
        counter--;
        VLOG(6) << "Counter value: " << counter;
    } else {
        LOG(ERROR) << "Invalid operation";
        exit(1);
    }

    version_hist.push_back({execute_idx, counter});

    response.set_value(counter);

    // std::string ret;
    // if (!response.SerializeToString(&ret)) {
    //     LOG(ERROR) << "Failed to serialize CounterResponse";
    //     throw std::runtime_error("Failed to serialize CounterResponse message.");
    //     return "";
    // }

    return response.SerializeAsString();
}

bool Counter::commit(uint32_t commit_idx)
{
    LOG(INFO) << "Committing counter value at idx: " << commit_idx;

    // Keep the most recent commit
    auto it = std::find_if(version_hist.rbegin(), version_hist.rend(), [commit_idx](const VersionedValue &v) {
        return v.version < commit_idx;
    });

    if (it != version_hist.rend()) {
        committed_state.value = it->value;
        committed_state.version = commit_idx;
        version_hist.erase(version_hist.begin(), it.base());

        LOG(INFO) << "Committed counter value: " << it->value;
    } else {
        LOG(INFO) << "No version needs to be cleaned up " << commit_idx;
        return true;
    }

    return true;
}

std::string Counter::getDigest(uint32_t digest_idx)
{
    // Find the latest versioned value at or before digest_idx
    auto it = std::find_if(version_hist.rbegin(), version_hist.rend(), [digest_idx](const VersionedValue &v) {
        return v.version <= digest_idx;
    });

    VersionedValue target;
    if (it != version_hist.rend()) {
        target = *it;
    } else {
        target = committed_state;
    }
    std::string digest = target.serialize();
    return digest;
}

std::shared_ptr<std::string> Counter::getSnapshot(uint32_t seq)
{
    LOG(INFO) << "Get counter snapshot(digest) at seq " << seq;
    return std::make_shared<std::string>(getDigest(seq));
}

void Counter::applyDelta(const std::string &snap)
{
    applySnapshot(snap);
}

void Counter::applySnapshot(const std::string &snap)
{
    committed_state = *((VersionedValue *) snap.c_str());
    counter = committed_state.value;
    version_hist.clear();

    LOG(INFO) << "Applying snapshot with value " << counter << " and version " << committed_state.version;
}

bool Counter::abort(const uint32_t abort_idx)
{
    LOG(INFO) << "Aborting operations after idx: " << abort_idx;
    if (abort_idx < committed_state.version) {
        // or revert to committed state?
        LOG(ERROR) << "Abort index is less than the committed state version " << committed_state.version
                   << ". Unable to revert.";
        return false;
    }
    if (abort_idx < version_hist.front().version) {
        version_hist.erase(version_hist.begin(), version_hist.end());
    } else if (abort_idx >= version_hist.back().version) {
        LOG(WARNING) << "Requested abort index is greater than the last version in history. No aborting";
        return true;
    } else {
        auto it = std::find_if(version_hist.rbegin(), version_hist.rend(), [abort_idx](const VersionedValue &v) {
            return v.version <= abort_idx;
        });
        if (it != version_hist.rend()) {
            // Erase all entries from the point found + 1 to the end entries from the point found to the end
            version_hist.erase(it.base(), version_hist.end());
        }
    }

    // Revert the counter to the last state not yet erased
    // for kv, fancier later.
    if (!version_hist.empty()) {
        counter = version_hist.back().value;
    } else {
        counter = committed_state.value;
    }
    LOG(INFO) << "Counter reverted to stable value: " << counter;

    return true;
}

void Counter::storeAppStateInYAML(const std::string &filename)
{
    std::map<std::string, std::string> app_state;
    app_state["counter"] = std::to_string(counter);
    app_state["committed_state"] = std::to_string(committed_state.value);
    app_state["committed_version"] = std::to_string(committed_state.version);
    // store to yaml file
    std::ofstream fout(filename);
    YAML::Node node;
    for (auto &kv : app_state) {
        node[kv.first] = kv.second;
    }
    fout << node;
    std::cout << "App state saved to " << filename << std::endl;
}

void *CounterTrafficGen::generateAppTraffic()
{
    CounterRequest *request = new CounterRequest();
    request->set_op(CounterOperation::INCREMENT);

    return request;
}
