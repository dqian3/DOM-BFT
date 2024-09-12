#include "processes/process_config.h"

#include "lib/message_type.h"
#include "lib/protocol_config.h"
#include "lib/signature_provider.h"
#include "lib/transport/address.h"
#include "lib/transport/endpoint.h"
#include "lib/utils.h"
#include "proto/dombft_proto.pb.h"
#include "lib/asyncLogger.h"    

#include <fstream>
#include <iostream>
#include <thread>

#include <yaml-cpp/yaml.h>

namespace dombft {
class Receiver {
private:
    SignatureProvider sigProvider_;

    /** The receiver uses this endpoint to receive requests from proxies and reply with OWD measurements*/
    std::unique_ptr<Endpoint> endpoint_;

    std::unique_ptr<Endpoint> forwardEp_;

    // a unique endpoint that is woken up only when logging pending or the logging timer expires
    std::unique_ptr<Endpoint> loggingEp_;

    std::map<std::string, std::thread> threads_;

    ConcurrentQueue<dombft::proto::DOMRequest> requestQueue_;

    // std::shared_ptr<AsyncLogger> asyncLogger_ = std::make_shared<AsyncLogger>();
    ConcurrentQueue<std::string> logQueue_;

    void LaunchThreads();

    void ReceiveTd();
    void ForwardTd();

    void LogTd();

    /** The handler objects for our endpoint library */
    // TODO shared pointer for endpoint and timer??
    std::unique_ptr<Timer> fwdTimer_;

    std::unique_ptr<Timer> heartbeatTimer_;

    std::unique_ptr<Timer> queueTimer_;

    std::unique_ptr<Timer> logTimer_;

    // TODO storing these protobuf objects like this might not be great performance wise
    // couldn't find much about this.
    std::map<std::pair<uint64_t, uint32_t>, dombft::proto::DOMRequest> deadlineQueue_;
    std::vector<dombft::proto::DOMRequest> lateMessages;

    /** The actual message / timeout handlers */
    void receiveRequest(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);

    void forwardRequest(const dombft::proto::DOMRequest &request);
    void checkDeadlines();

    void addToDeadlineQueue();

    void flushLogs();

    uint32_t receiverId_;
    uint32_t proxyMeasurementPort_;
    uint32_t numReceivers_;
    Address replicaAddr_;

    // Skip forwarding, for running experiemnts.
    bool skipForwarding_;
    bool ignoreDeadlines_;

public:
    Receiver(const ProcessConfig &config, uint32_t receiverId, bool skipForwarding = false,
             bool ignoreDeadlines_ = false);
    ~Receiver();
    void run();
};

}   // namespace dombft