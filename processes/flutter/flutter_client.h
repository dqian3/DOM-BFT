#pragma once

#include "lib/config/config_manager.h"

#include <span>
#include <thread>
#include <unordered_map>

#include "lib/common.h"
#include "lib/crypto/hmac_provider.h"
#include "lib/crypto/sig_provider.h"
#include "lib/threadpool.h"
#include "lib/transport/address.h"
#include "lib/transport/endpoint.h"
#include "lib/utils.h"

#include "proto/dombft_proto.pb.h"
#include "proto/flutter_proto.pb.h"

#include <yaml-cpp/yaml.h>

namespace flutter {

struct FlutterRequestState {
    flutter::proto::FlutterClientRequest request;
    uint32_t clientSeq;
    uint64_t bet;

    uint64_t submitTime;
    uint64_t numRetries = 0;
    bool completed = false;

    // Vote tracking for f+1 consensus
    uint32_t acceptVotes = 0;
    uint32_t rejectVotes = 0;
    std::set<uint32_t> votedReplicas;   // Track which replicas have voted

    FlutterRequestState(const flutter::proto::FlutterClientRequest &req, uint64_t bet)
        : request(req)
        , clientSeq(req.client_seq())
        , bet(bet)
        , submitTime(GetMicrosecondTimestamp())
    {
    }
};

enum ClientSendMode { RateBased = 0, MaxInFlightBased = 1 };

class FlutterClient {
private:
    uint32_t clientId_;
    std::vector<Address> replicaAddrs_;

    // Cryptography
    SignatureProvider sigProvider_;
    HMACProvider hmacProvider_;
    bool useHMAC_;

    // Network
    std::unique_ptr<Endpoint> endpoint_;
    ThreadPool sendThreadpool_;

    // Request management
    std::unordered_map<uint32_t, std::unique_ptr<FlutterRequestState>> pendingRequests_;
    uint32_t nextSeq_ = 1;

    // Configuration
    uint64_t baseBetOffset_;
    uint64_t betIncrement_;
    uint32_t f_;
    uint32_t requestSize_;

    // Send control
    ClientSendMode sendMode_;
    uint32_t sendRate_;
    uint32_t maxInFlight_;
    uint32_t numInFlight_ = 0;
    uint64_t lastSendTime_ = 0;
    bool firstRequestCommitted_ = false;
    uint64_t startTime_ = 0;

    // Timers
    std::unique_ptr<Timer> sendTimer_;
    std::unique_ptr<Timer> terminateTimer_;

    bool running_;

    void handleMessage(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);
    void processFlutterReply(const flutter::proto::FlutterReply &reply);
    void sendRequest(FlutterRequestState &state);
    void submitRequestsOpenLoop();
    void commitRequest(uint32_t clientSeq);

public:
    FlutterClient(uint32_t clientId, uint64_t baseBetOffset, uint64_t betIncrement);
    ~FlutterClient();

    void submitRequest(const std::string &data);
    void run();
    void stop();
};

}   // namespace flutter