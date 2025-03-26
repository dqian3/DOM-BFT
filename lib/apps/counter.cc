#include "counter.h"

#include "proto/dombft_apps.pb.h"

using namespace dombft::apps;

Counter::~Counter() {}

std::string Counter::execute(const std::string &serialized_request, uint32_t execute_idx)
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
        return v.version <= commit_idx;
    });

    if (it != version_hist.rend()) {
        committed_state.value = it->value;
        committed_state.version = commit_idx;
        version_hist.erase(version_hist.begin(), it.base() + 1);

        LOG(INFO) << "Committed counter value: " << it->value;
    } else {
        LOG(INFO) << "No version needs to be cleaned up " << commit_idx;
        return true;
    }

    return true;
}

bool Counter::abort(uint32_t abort_idx)
{
    LOG(INFO) << "Aborting operations starting from idx: " << abort_idx;
    if (abort_idx < committed_state.version) {
        LOG(ERROR) << "Abort index is less than the committed state version " << committed_state.version
                   << ". Unable to revert.";
        return false;
    }

    for (auto it = version_hist.begin(); it != version_hist.end();) {
        if (it->version >= abort_idx) {
            it = version_hist.erase(it);
        } else {
            ++it;
        }
    }

    // Revert the counter to the last state not yet erased
    if (!version_hist.empty()) {
        counter = version_hist.back().value;
    } else {
        counter = committed_state.value;
    }
    LOG(INFO) << "Counter reverted to stable value: " << counter;

    return true;
}

bool Counter::applyDelta(const std::string &snap, const std::string &digest) { return applySnapshot(snap, digest); }

bool Counter::applySnapshot(const std::string &snap, const std::string &digest)
{
    // get the element seperated by ,
    std::string version_str = snap.substr(0, snap.find(','));
    std::string value_str = snap.substr(snap.find(',') + 1);
    committed_state.version = std::stoull(version_str);
    committed_state.value = std::stoi(value_str);
    counter = committed_state.value;
    version_hist.clear();

    LOG(INFO) << "Applying snapshot with value " << counter << " and version " << committed_state.version;
    return true;
}

::AppSnapshot Counter::takeSnapshot()
{
    ::AppSnapshot ret;
    uint32_t idx, snapshot;
    if (version_hist.empty()) {
        ret.snapshot = std::to_string(committed_state.version) + "," + std::to_string(committed_state.value);
        ret.idx = committed_state.version;
    } else {
        ret.snapshot = std::to_string(version_hist.back().version) + "," + std::to_string(version_hist.back().value);
        ret.idx = version_hist.back().version;
    }

    ret.digest = ret.snapshot;
    ret.delta = ret.snapshot;
    ret.fromIdxDelta = committed_state.version;

    return ret;
}

std::string CounterClient::generateAppRequest()
{
    CounterRequest request;
    request.set_op(CounterOperation::INCREMENT);

    return request.SerializeAsString();
}
