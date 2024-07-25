#include "counter.h"

#include "proto/dombft_apps.pb.h"


using namespace dombft::apps;

Counter::~Counter() {
}

std::unique_ptr<AppLayerResponse> Counter::execute(const std::string &serialized_request)
{    
    LOG(INFO) << "Received request to execute counter operation";

    std::unique_ptr<CounterRequest> counterReq = std::make_unique<CounterRequest>();
    if (!counterReq->ParseFromString(serialized_request)) {
        LOG(ERROR) << "Failed to parse CounterRequest";
        return nullptr; 
    }

    std::unique_ptr<AppLayerResponse> ret = std::make_unique<AppLayerResponse>();

    CounterOperation op = counterReq->op();

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

    ret->success = true;
    ret->response = std::make_unique<byte[]>(INT_SIZE_IN_BYTES);

    ret->result_len = INT_SIZE_IN_BYTES;

    std::memcpy(ret->response.get(), &counter, INT_SIZE_IN_BYTES);

    int return_value;
    std::memcpy(&return_value, ret->response.get(), INT_SIZE_IN_BYTES);
    LOG(INFO) << "Return value: " << return_value;

    return ret;
}

bool Counter::commit(uint32_t commit_idx, byte* committed_value)
{
    LOG(INFO) << "Committing counter value at idx: " << commit_idx;

    int value = *reinterpret_cast<int*>(committed_value);

    counter_stable = value;

    LOG(INFO) << "Committed counter value: " << value;

    return true;
}

byte* Counter::getDigest(uint32_t digest_idx)
{
    std::copy(static_cast<const char*>(static_cast<const void*>(&counter)),
              static_cast<const char*>(static_cast<const void*>(&counter)) + INT_SIZE_IN_BYTES,
              commit_digest);

    return commit_digest;
}

byte* Counter::takeSnapshot()
{
    std::copy(static_cast<const char*>(static_cast<const void*>(&counter_stable)),
              static_cast<const char*>(static_cast<const void*>(&counter_stable)) + INT_SIZE_IN_BYTES,
              snapshot_digest);

    return snapshot_digest;
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

