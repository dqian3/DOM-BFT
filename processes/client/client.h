#include "processes/process_config.h"

#include <fstream>
#include <iostream>
#include <optional>
#include <thread>

#include "lib/message_type.h"
#include "lib/protocol_config.h"
#include "lib/signature_provider.h"
#include "lib/transport/address.h"
#include "lib/transport/udp_endpoint.h"
#include "lib/utils.h"
#include "proto/dombft_proto.pb.h"

#include <yaml-cpp/yaml.h>

namespace dombft {
struct RequestState {
    std::map<int, dombft::proto::Reply> replies;
    std::map<int, std::string> signatures;
    std::optional<dombft::proto::Cert> cert;
    uint64_t sendTime;
    uint64_t certTime;
    std::set<int> certReplies;

    uint32_t instance = 0;

    std::map<int, dombft::proto::FallbackExecuted> fallbackReplies;
    std::optional<dombft::proto::Cert> fallbackProof;
    uint32_t fallbackAttempts = 0;
    bool fastPathPossible = true;
};

enum clientSendMode 
{
    rateBased = 0,
    maxInFlightBased = 1
};

enum backpressureMode
{
    none = 0,
    sleep = 1,
    adjust = 2
};

class Client {
private:
    /* Config parameters that need to be saved */
    uint32_t clientId_;
    std::vector<Address> proxyAddrs_;
    std::vector<Address> replicaAddrs_;
    uint32_t f_;
    uint32_t maxInFlight_ = 0;
    uint32_t numRequests_;
    uint32_t numCommitted_ = 0;

    uint32_t clientSendRate_;

    uint64_t normalPathTimeout_;
    uint64_t slowPathTimeout_;

    /** The endpoint uses to submit request to proxies and receive replies*/
    std::unique_ptr<Endpoint> endpoint_;
    /** Timer to handle request timeouts  (TODO timeouts vs repeated timer would maybe be better)*/
    std::unique_ptr<Timer> timeoutTimer_;

    // timer to control sending rate of the client
    std::unique_ptr<Timer> sendTimer_;

    std::unique_ptr<Timer> restartSendTimer_;

    dombft::clientSendMode sendMode_;

    /** Timer to stop client after running for configured time */
    std::unique_ptr<Timer> terminateTimer_;

    SignatureProvider sigProvider_;

    /** The next requestId to be submitted */
    uint32_t nextReqSeq_ = 0;
    uint32_t inFlight_ = 0;
    uint32_t numExecuted_ = 0;

    dombft::backpressureMode backpressureMode_;
    double clientBackPressureSleepTime;

    /* State for the currently pending request */
    std::map<int, RequestState> requestStates_;

    /** The message handler to handle messages */
    void receiveReply(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);
    void checkReqState(uint32_t client_seq);

    void submitRequest();

    void checkTimeouts();

    void adjustSendRate();

public:
    /** Client accepts a config file, which contains all the necessary information
     * to instantiate the object, then it can call Run method
     *  */
    Client(const ProcessConfig &config, const size_t clientId);
    ~Client();

    void run();
};

}   // namespace dombft