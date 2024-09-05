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

    std::string ret;
    if (!response.SerializeToString(&ret)) {
        LOG(ERROR) << "Failed to serialize CounterResponse";
        throw std::runtime_error("Failed to serialize CounterResponse message.");
        return "";
    }

    return ret;
}

bool Counter::commit(uint32_t commit_idx)
{
    LOG(INFO) << "Committing counter value at idx: " << commit_idx;

    auto it = std::find_if(version_hist.rbegin(), version_hist.rend(),
                           [commit_idx](const VersionedValue &v) { return v.version <= commit_idx; });

    if (it != version_hist.rend()) {
        counter_stable = it->value;
        version_hist.erase(version_hist.begin(), it.base());

        LOG(INFO) << "Committed counter value: " << counter_stable;
    } else {
        LOG(INFO) << "No version needs to be cleaned up " << commit_idx;
        return true;
    }

    return true;
}

std::string Counter::getDigest(uint32_t digest_idx)
{
    // Find the latest versioned value at or before digest_idx
    auto it = std::find_if(version_hist.rbegin(), version_hist.rend(),
                           [digest_idx](const VersionedValue &v) { return v.version <= digest_idx; });

    int value_digest;

    if (it != version_hist.rend()) {
        value_digest = it->value;
    } else {
        value_digest = counter_stable;
    }
    return std::string(reinterpret_cast<const char *>(&counter), INT_SIZE_IN_BYTES);
}

std::string Counter::takeSnapshot()
{
    return std::string(reinterpret_cast<const char *>(&counter_stable), INT_SIZE_IN_BYTES);
}

void *CounterTrafficGen::generateAppTraffic()
{
    CounterRequest *request = new CounterRequest();
    request->set_op(CounterOperation::INCREMENT);

    return request;
}

bool Counter::abort(const uint32_t abort_idx)
{

    LOG(INFO) << "Aborting operations after idx: " << abort_idx;

    // Find the last committed value before or at abort_idx
    auto it = std::find_if(version_hist.rbegin(), version_hist.rend(),
                           [abort_idx](const VersionedValue &v) { return v.version <= abort_idx; });

    if (it != version_hist.rend()) {
        // Erase all entries from the point found to the end
        version_hist.erase(it.base(), version_hist.end());
    }

    // Revert the counter to the last state not yet erased
    // for kv, fancier later.
    if (!version_hist.empty()) {
        counter = version_hist.back().value;
    } else {
        counter = counter_stable;
    }

    LOG(INFO) << "Counter reverted to stable value: " << counter_stable;

    return true;
}