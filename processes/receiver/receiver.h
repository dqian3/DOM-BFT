#include "processes/process_config.h"

#include "lib/message_type.h"
#include "lib/protocol_config.h"
#include "lib/signature_provider.h"
#include "lib/transport/address.h"
#include "lib/transport/endpoint.h"
#include "lib/utils.h"
#include "proto/dombft_proto.pb.h"

#include <fstream>
#include <iostream>
#include <thread>

#include <yaml-cpp/yaml.h>

namespace dombft {
class Receiver {
private:
    /** Each thread is given a unique name (key) */
    std::map<std::string, std::thread *> threads_;

    /** Launch threads:
     * (1) receiveRequest: receive the L7 proxy forwarding from the proxiy
     * and put the content in a concurrent queue
     * (2) forwardRequest: relay the requests to the replicas when the deadline hits. 
     */
    void LaunchThreads();

    ConcurrentQueue<dombft::proto::DOMRequest> requestsQueue_;

    SignatureProvider sigProvider_;

    /** The receiver uses this endpoint to receive requests from proxies and reply with OWD measurements*/
    std::unique_ptr<Endpoint> recvReqEp_;

    // this endpoint is used for sending out timed out requests who have reached the deadline
    std::unique_ptr<Endpoint> forwardReqEp_;

    /** The handler objects for our endpoint library */
    // TODO shared pointer for endpoint and timer??
    std::unique_ptr<Timer> fwdTimer_;

    // the timer to periodically check the concurrent queue for new message. 
    std::unique_ptr<Timer> checkQueueTimer_;

    // TODO storing these protobuf objects like this might not be great performance wise
    // couldn't find much about this.
    std::map<std::pair<uint64_t, uint32_t>, dombft::proto::DOMRequest> deadlineQueue_;
    std::vector<dombft::proto::DOMRequest> lateMessages;

    void receiveRequestTd();
    void forwardRequestTd();

    // fetch periodically from the recv queue and store it into the hash map. 
    void checkRecvQueue();

    // flag to run/terminate the threads:
    std::atomic<bool> running_;

    /** The actual message / timeout handlers */
    void receiveRequest(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);

    void forwardRequest(const dombft::proto::DOMRequest &request);
    void checkDeadlines();

    uint32_t receiverId_;
    uint32_t proxyMeasurementPort_;
    Address replicaAddr_;

public:
    Receiver(const ProcessConfig &config, uint32_t receiverId);
    ~Receiver();
    void run();
    void terminate();
};

}   // namespace dombft