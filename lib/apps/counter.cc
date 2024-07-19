#include "counter.h"

#include "proto/dombft_apps.pb.h"


using namespace dombft::apps;

Counter::~Counter() {
}

std::unique_ptr<AppResponse> Counter::execute(const AppRequest &request)
{    
    CounterRequest *counterReq = (CounterRequest *) &request;

    std::unique_ptr<CounterResponse> ret = std::make_unique<CounterResponse>();

    CounterOperation op = counterReq->op();

    if (op == CounterOperation::INCREMENT) {
        counter++;
        ret->set_value(counter);
    } else if (op == CounterOperation::DECREMENT) {
        counter--;
        ret->set_value(counter);
    } else {
        LOG(ERROR) << "Invalid operation";
        exit(1);
    }

    return ret;
}

bool Counter::commit(uint32_t commit_idx)
{
    byte* result = log_->getEntry(commit_idx)->raw_result;
    if (result == nullptr) {
        LOG(WARNING) << "No result to commit";
        return false;
    }

    int value = *reinterpret_cast<int*>(result);
    counter_stable = value;

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

