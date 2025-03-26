#include "counter.h"

#include "proto/dombft_apps.pb.h"

using namespace dombft::apps;

Counter::~Counter() {}

std::string Counter::execute(const std::string &serialized_request, uint32_t execute_idx)
{
    dombft::apps::CounterRequest counterReq;
    if (!counterReq.ParseFromString(serialized_request)) {
        LOG(ERROR) << "Failed to parse CounterRequest";
        exit(1);
    }

    CounterOperation op = counterReq.op();
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

    response.set_value(counter);

    return response.SerializeAsString();
}

bool Counter::commit(uint32_t idx)
{
    LOG(INFO) << "Committing counter value at idx: " << idx;

    committedIdx = idx;
    committedValue = counter;

    return true;
}

bool Counter::abort(uint32_t idx)
{
    counter = idx;

    return true;
}

bool Counter::applyDelta(const std::string &snap, const std::string &digest) { return applySnapshot(snap, digest); }

bool Counter::applySnapshot(const std::string &snap, const std::string &digest)
{
    // get the element seperated by ,

    VLOG(1) << "Applying snapshot: '" << snap << "'";
    std::string idxStr = snap.substr(0, snap.find(','));
    std::string valueStr = snap.substr(snap.find(',') + 1);

    committedIdx = std::stoull(idxStr);
    committedValue = std::stoi(valueStr);

    counter = committedValue;

    LOG(INFO) << "Applying snapshot with value " << counter << " and index " << committedIdx;
    return true;
}

::AppSnapshot Counter::takeSnapshot()
{
    ::AppSnapshot ret;

    ret.fromIdxDelta = 0;
    ret.idx = 0;

    ret.snapshot = std::to_string(counter) + "," + std::to_string(counter);
    ret.idx = counter;

    ret.digest = ret.snapshot;
    ret.delta = ret.snapshot;
    ret.fromIdxDelta = counter;

    return ret;
}

std::string CounterClient::generateAppRequest()
{
    CounterRequest request;
    request.set_op(CounterOperation::INCREMENT);

    return request.SerializeAsString();
}
