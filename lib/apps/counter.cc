#include "counter.h"

#include "proto/dombft_apps.pb.h"


using namespace dombft::apps;

Counter::~Counter() {
}

std::string Counter::execute(const std::string &serialized_request, const uint32_t execute_idx)
{    
    LOG(INFO) << "Received request to execute counter operation";

    std::unique_ptr<CounterRequest> counterReq = std::make_unique<CounterRequest>();
    if (!counterReq->ParseFromString(serialized_request)) {
        LOG(ERROR) << "Failed to parse CounterRequest";
        return ""; 
    }

    CounterOperation op = counterReq->op();
    CounterResponse response;

    if (op == CounterOperation::INCREMENT) {
        LOG(INFO) << "Incrementing counter";
        counter++;
        LOG(INFO) << "Counter value: " << counter;
    } else if (op == CounterOperation::DECREMENT) {
        LOG(INFO) << "Decrementing counter";
        counter--;
        LOG(INFO) << "Counter value: " << counter;
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
        [commit_idx](const VersionedValue& v) { return v.version <= commit_idx; }); 

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

    return std::string(reinterpret_cast<const char*>(&counter), INT_SIZE_IN_BYTES);
}

std::string Counter::takeSnapshot()
{
    return std::string(reinterpret_cast<const char*>(&counter_stable), INT_SIZE_IN_BYTES);
}

void* CounterTrafficGen::generateAppTraffic()
{
    CounterRequest *request = new CounterRequest();
    request->set_op(CounterOperation::INCREMENT);

    return request;
}

bool Counter::abort()
{
    counter = counter_stable;
    return true;
}

