#include "processes/process_config.h"

#include "lib/message_type.h"
#include "lib/protocol_config.h"
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
    uint64_t lastFwdDeadline;

    // Queue for worker threads to trigger verify tasks through
    // ConcurrentQueue<std::shared_ptr<Request>> verifyQueue_;

    // Queue for worker threads to trigger verify tasks through
    std::mutex verifyQueueMtx_;
    std::condition_variable verifyCondVar_;
    std::queue<std::shared_ptr<Request>> verifyQueue_;

    std::vector<std::thread> verifyThds_;

    /** The actual message / timeout handlers */
    void receiveRequest(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);

    void checkDeadlines();
    void forwardRequest(const dombft::proto::DOMRequest &request);

    void verifyWorker();

    uint32_t receiverId_;
    uint32_t proxyMeasurementPort_;
    uint32_t numReceivers_;
    Address replicaAddr_;

    // Turn off various receiver behaviors, for running micro-experiments between proxy and receiver
    bool skipForwarding_;
    bool ignoreDeadlines_;
    bool skipVerify_;

    bool running_;

public:
    Receiver(const ProcessConfig &config, uint32_t receiverId, bool skipForwarding = false,
             bool ignoreDeadlines = false, bool skipVerify = false);
    ~Receiver();
};

}   // namespace dombft