#include "counter.h"

#include "proto/dombft_apps.pb.h"


using namespace dombft::apps;

Counter::~Counter() {
}

std::unique_ptr<AppResponse> Counter::execute(const std::string &serialized_request)
{    
    std::unique_ptr<CounterRequest> counterReq = std::make_unique<CounterRequest>();
    if (!counterReq->ParseFromString(serialized_request)) {
        LOG(ERROR) << "Failed to parse CounterRequest";
        return nullptr; 
    }

    std::unique_ptr<CounterResponse> ret = std::make_unique<CounterResponse>();

    CounterOperation op = counterReq->op();

    if (op == CounterOperation::INCREMENT) {
        LOG(INFO) << "Incrementing counter";
        counter++;
        ret->set_value(counter);
        LOG(INFO) << "Counter value: " << counter;
    } else if (op == CounterOperation::DECREMENT) {
        LOG(INFO) << "Decrementing counter";
        counter--;
        ret->set_value(counter);
        LOG(INFO) << "Counter value: " << counter;
    } else {
        LOG(ERROR) << "Invalid operation";
        exit(1);
    }

    log_->getEntry(log_->nextSeq-1)->raw_result = new byte[sizeof(int)];
    *reinterpret_cast<int*>(log_->getEntry(log_->nextSeq-1)->raw_result) = counter;

    return ret;
}

bool Counter::commit(uint32_t commit_idx)
{
    LOG(INFO) << "Committing counter value at idx: " << commit_idx;
    byte* result = log_->getEntry(commit_idx)->raw_result;
    if (result == nullptr) {
        LOG(WARNING) << "No result to commit";
        return false;
    }

    int value = *reinterpret_cast<int*>(result);
    counter_stable = value;

    LOG(INFO) << "Committed counter value: " << counter_stable;

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

