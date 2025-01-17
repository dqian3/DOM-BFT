#include "processes/process_config.h"

#include <optional>
#include <span>
#include <thread>

#include "lib/cert_collector.h"
#include "lib/common.h"
#include "lib/signature_provider.h"
#include "lib/threadpool.h"
#include "lib/transport/address.h"
#include "lib/transport/udp_endpoint.h"
#include "lib/utils.h"
#include "proto/dombft_proto.pb.h"

#include <yaml-cpp/yaml.h>

namespace dombft {
struct RequestState {

    RequestState(uint32_t f, dombft::proto::ClientRequest &req, uint64_t sendT)
        : collector(f)
        , request(req)
        , client_seq(req.client_seq())
        , sendTime(sendT)
    {
    }
    CertCollector collector;
    dombft::proto::ClientRequest request;

    uint32_t client_seq;
    uint64_t sendTime;

    // Normal path state
    uint64_t certTime;
    bool certSent = false;
    uint64_t certSendTime = false;
    std::set<int> certReplies;

    // Slow Path state
    bool triggerSent = false;
    uint64_t triggerSendTime;
    // TODO keep track of matching replies, not just number of replies, of which we need f + 1
    std::set<int> fallbackReplies;
    std::optional<dombft::proto::Cert> fallbackProof;
};

enum ClientSendMode { RateBased = 0, MaxInFlightBased = 1 };

enum BackpressureMode { None = 0, Sleep = 1, Adjust = 2 };

class Client {
private:
    /* Config parameters that need to be saved */
    uint32_t clientId_;
    std::vector<Address> proxyAddrs_;
    std::vector<Address> replicaAddrs_;
    uint32_t f_;

    /* Sending config */
    dombft::ClientSendMode sendMode_;
    uint32_t sendRate_;
    uint32_t maxInFlight_ = 0;

    uint64_t normalPathTimeout_;
    uint64_t slowPathTimeout_;

    /** The endpoint uses to submit request to proxies and receive replies*/
    std::unique_ptr<Endpoint> endpoint_;
    /** Timer to handle request timeouts  (TODO timeouts vs repeated timer would maybe be better)*/
    std::unique_ptr<Timer> timeoutTimer_;

    // timer to control sending rate of the client
    std::unique_ptr<Timer> sendTimer_;

    /** Timer to stop client after running for configured time */
    std::unique_ptr<Timer> terminateTimer_;

    /* Class for generating requests */
    std::unique_ptr<AppTrafficGen> trafficGen_;
    AppType appType_;

    SignatureProvider sigProvider_;
    ThreadPool threadpool_;
    std::mutex clientStateLock;

    /* Global state */

    // Map of replica id instance, once f + 1 are higher than n, update own instance
    std::map<uint32_t, uint32_t> replicaInstances_;
    std::map<uint32_t, uint32_t> replicaViews_;
    uint32_t myInstance_ = 0;
    uint32_t myView_ = 0;

    // Keeping track of sending rate
    uint64_t lastSendTime_ = 0;

    uint32_t nextSeq_ = 0;
    uint32_t numInFlight_ = 0;
    uint32_t numCommitted_ = 0;

    uint32_t lastCommitted_ = 0;
    uint32_t lastFastPath_ = 0;
    uint32_t lastNormalPath_ = 0;
    uint32_t lastSlowPath_ = 0;

    uint64_t startTime_ = 0;

    /* Per request state */
    std::map<uint32_t, RequestState> requestStates_;

    /** The message handler to handle messages */
    void handleMessage(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);
    void handleReply(dombft::proto::Reply &reply, std::span<byte> sig);
    void handleCertReply(const dombft::proto::CertReply &reply, std::span<byte> sig);
    void handleFallbackSummary(const dombft::proto::FallbackSummary &summary, std::span<byte> sig);

    void submitRequest();
    void submitRequestsOpenLoop();   // For sending in open loop.

    void retryRequests();
    void sendRequest(const dombft::proto::ClientRequest &request, byte *sendBuffer = nullptr);
    void commitRequest(uint32_t clientSeq);

    bool updateInstance();
    bool updateView();

    void checkTimeouts();

public:
    /** Client accepts a config file, which contains all the necessary information
     * to instantiate the object, then it can call Run method
     *  */
    Client(const ProcessConfig &config, const size_t clientId);
    ~Client();
};

}   // namespace dombft