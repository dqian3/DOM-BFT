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
    byte* digest = log_->getEntry(digest_idx)->digest;
    return digest;
}

byte* Counter::takeSnapshot()
{
    return getDigest(log_->lastExecuted);
}

