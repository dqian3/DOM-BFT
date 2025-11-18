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

#include "proto/flutter_proto.pb.h"
#include "proto/dombft_proto.pb.h"

#include <yaml-cpp/yaml.h>

namespace dombft {

struct FlutterRequestState {
    flutter::proto::FlutterClientRequest request;
    uint32_t clientSeq;
    uint64_t currentBet;
    uint64_t baseBet;
    uint64_t betIncrement;
    uint64_t sendTime;
    bool completed = false;

    FlutterRequestState(const flutter::proto::FlutterClientRequest& req, uint64_t bet, uint64_t increment)
        : request(req)
        , clientSeq(req.client_seq())
        , currentBet(bet)
        , baseBet(bet)
        , betIncrement(increment)
        , sendTime(GetMicrosecondTimestamp())
    {}
};

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
    uint64_t baseBet_;
    uint64_t betIncrement_;
    uint32_t maxRetries_;

    bool running_;
    std::vector<std::thread> threads_;

    void handleMessage(MessageHeader* msgHdr, byte* msgBuffer, Address* sender);
    void processFlutterReply(const flutter::proto::FlutterReply& reply);
    void retryRequest(FlutterRequestState& state);
    void sendRequest(FlutterRequestState& state);

public:
    FlutterClient(uint32_t clientId, uint64_t baseBet, uint64_t betIncrement = 1000, uint32_t maxRetries = 10);
    ~FlutterClient();

    void submitRequest(const std::string& data);
    void run();
    void stop();
};

} // namespace dombft