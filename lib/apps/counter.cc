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

    values[execute_idx] = counter;
    response.set_value(counter);

    return response.SerializeAsString();
}

bool Counter::commit(uint32_t idx)
{
    LOG(INFO) << "Committing counter value at idx: " << idx;

    auto it = values.lower_bound(idx);

    committedIdx = it->first;
    committedValue = it->second;

    values.erase(values.begin(), it);

    return true;
}

bool Counter::abort(uint32_t idx)
{
    values.erase(values.lower_bound(idx), values.end());

    if (values.empty()) {
        counter = committedValue;
    } else {
        counter = values.rbegin()->second;
    }

    return true;
}

bool Counter::applySnapshot(const std::string &snap, const std::string &digest, uint32_t idx)
{
    // get the element seperated by ,

    VLOG(1) << "Applying snapshot: '" << snap << "'";
    std::string idxStr = snap.substr(0, snap.find(','));
    std::string valueStr = snap.substr(snap.find(',') + 1);

    committedIdx = std::stoull(idxStr);
    committedValue = std::stoi(valueStr);

    assert(committedIdx == idx);

    counter = committedValue;
    values.clear();

    LOG(INFO) << "Applying snapshot with value " << counter << " and index " << committedIdx;
    return true;
}

AppSnapshot Counter::getSnapshot()
{
    AppSnapshot ret;

    if (values.empty()) {
        ret.seq = committedIdx;
        ret.snapshot =
            std::make_shared<std::string>(std::to_string(committedIdx) + "," + std::to_string(committedValue));
    } else {
        auto lastEntry = values.rbegin();
        ret.seq = lastEntry->first;
        ret.snapshot =
            std::make_shared<std::string>(std::to_string(lastEntry->first) + "," + std::to_string(lastEntry->second));
    }
    VLOG(1) << "Creating snapshot: '" << *ret.snapshot << "'";

    ret.digest = *ret.snapshot;
    return ret;
}

std::string CounterClient::generateAppRequest()
{
    CounterRequest request;
    request.set_op(CounterOperation::INCREMENT);

    return request.SerializeAsString();
}
