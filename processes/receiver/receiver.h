#include "processes/process_config.h"

#include "lib/common.h"
#include "lib/signature_provider.h"
#include "lib/transport/address.h"
#include "lib/transport/endpoint.h"
#include "lib/utils.h"
#include "proto/dombft_proto.pb.h"

#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>

#include <yaml-cpp/yaml.h>

struct Request {
    dombft::proto::DOMRequest request;
    uint64_t deadline;
    uint32_t clientId;

    bool verified = false;
};

namespace dombft {
class Receiver {
private:
    SignatureProvider sigProvider_;

    /** The receiver uses this endpoint to receive requests from proxies and reply with OWD measurements*/
    std::unique_ptr<Endpoint> endpoint_;

    /** Timer to trigger checking deadlineQueue to forward messages */
    std::unique_ptr<Timer> fwdTimer_;

    // Map for requests to be forwarded in deadline order
    std::mutex deadlineQueueMtx_;
    std::map<std::pair<uint64_t, uint32_t>, std::shared_ptr<Request>> deadlineQueue_;

    // Queue for worker threads to trigger verify tasks through
    BlockingConcurrentQueue<std::shared_ptr<Request>> verifyQueue_;

    std::vector<std::thread> verifyThds_;
    std::thread forwardThd_;
    bool running_;

    // Static receiver config
    uint32_t receiverId_;
    uint32_t proxyMeasurementPort_;
    uint32_t numReceivers_;
    Address replicaAddr_;

    // Bookeeping
    uint64_t lastFwdDeadline_ = 0;
    // Map from proxy_id to last measurement sent time
    std::map<uint32_t, uint64_t> lastMeasurementTimes_;
    uint32_t numForwarded_ = 0;
    uint64_t lastStatTime_ = 0;

    // Turn off various receiver behaviors, for running micro-experiments between proxy and receiver
    bool skipForwarding_;
    bool ignoreDeadlines_;

    /** The actual message  handlers */
    void receiveRequest(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);

    void verifyThd(int threadId);

    /* Pushing requests off the queue */
    void forwardThd();
    uint64_t checkDeadlines();   // Returns the time until the next deadline
    void forwardRequest(const dombft::proto::DOMRequest &request);

    void setCpuAffinites(const std::set<int> &processingSet, const std::set<int> &verifySet);

public:
    Receiver(
        const ProcessConfig &config, uint32_t receiverId, bool skipForwarding = false, bool ignoreDeadlines = false
    );
    ~Receiver();
};

}   // namespace dombft