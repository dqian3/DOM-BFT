#include "flutter_client.h"

#include "lib/config/config_util.h"
#include "lib/transport/nng_endpoint_threaded.h"
#include "lib/transport/ooo_rpc_endpoint.h"
#include "lib/transport/udp_endpoint.h"

#include <chrono>

namespace flutter {

FlutterClient::FlutterClient(uint32_t clientId, uint64_t baseBetOffset, uint64_t betIncrement)
    : clientId_(clientId)
    , sendThreadpool_(4)   // 4 threads for sending
    , baseBetOffset_(baseBetOffset)
    , betIncrement_(betIncrement)
    , running_(false)
{
    auto &configManager = dombft::ConfigManager::getInstance();
    const auto &config = configManager.getConfig();

    useHMAC_ = config.clientUseHMAC;
    f_ = config.f;
    maxInFlight_ = config.clientMaxInFlight;
    sendRate_ = config.clientSendRate;
    requestSize_ = config.clientRequestSize;

    if (config.clientSendMode == "sendRate") {
        sendMode_ = flutter::RateBased;
        LOG(INFO) << "Flutter Client using rate-based sending at " << sendRate_ << " req/s";
    } else if (config.clientSendMode == "maxInFlight") {
        sendMode_ = flutter::MaxInFlightBased;
        LOG(INFO) << "Flutter Client using maxInFlight-based sending with " << maxInFlight_ << " in flight";
    } else {
        LOG(ERROR) << "Unknown send mode: " << config.clientSendMode;
        exit(1);
    }

    // Load cryptographic keys
    std::string clientKey = config.clientKeysDir + "/client" + std::to_string(clientId_) + ".der";
    LOG(INFO) << "Loading client key from " << clientKey;
    if (!sigProvider_.loadPrivateKey(clientKey)) {
        LOG(ERROR) << "Unable to load client private key!";
        exit(1);
    }

    if (!sigProvider_.loadPublicKeys(NodeType::REPLICA, config.replicaKeysDir)) {
        LOG(ERROR) << "Unable to load replica public keys!";
        exit(1);
    }

    if (useHMAC_) {
        hmacProvider_.loadClientKeysDev({NodeType::CLIENT, clientId_}, configManager.getNumReplicas());
    }

    // Setup network addresses
    const auto &clientIps = configManager.getClientIps();
    std::string clientIp = clientIps[clientId_];
    int clientPort = configManager.getClientPort() + clientId_;

    LOG(INFO) << "Flutter Client " << clientId_ << " binding to " << clientIp << ":" << clientPort;

    if (config.transport == "udp") {
        const auto &replicaIps = configManager.getReplicaIps();
        for (size_t i = 0; i < replicaIps.size(); i++) {
            replicaAddrs_.push_back(Address(replicaIps[i], configManager.getReplicaPort()));
        }
        endpoint_ = std::make_unique<UDPEndpoint>(clientIp, clientPort);
    } else if (config.transport == "nng") {
        auto addrPairs = getClientAddrs(config, clientId_);
        size_t numReplicas = configManager.getNumReplicas();

        for (size_t i = 0; i < numReplicas; i++) {
            replicaAddrs_.push_back(addrPairs[i].second);
        }

        endpoint_ = std::make_unique<NngEndpointThreaded>(addrPairs, false, Address(clientIp, clientPort));
    } else if (config.transport == "simple-rpc") {
        std::vector<Address> allAddrs;

        const auto &replicaIps = configManager.getReplicaIps();
        for (size_t i = 0; i < replicaIps.size(); i++) {
            replicaAddrs_.push_back(Address(replicaIps[i], configManager.getReplicaPort()));
            allAddrs.push_back(replicaAddrs_.back());
        }

        allAddrs.push_back(Address(clientIp, clientPort));
        endpoint_ = std::make_unique<OOORPCEndpoint>(clientIp, clientPort, allAddrs, sendThreadpool_.size());
    } else {
        LOG(ERROR) << "Unsupported transport " << config.transport;
        exit(1);
    }

    // Setup message handler
    MessageHandlerFunc handler = [this](MessageHeader *msgHdr, byte *msgBuffer, Address *sender) {
        this->handleMessage(msgHdr, msgBuffer, sender);
        if (sendMode_ == flutter::RateBased) {
            submitRequestsOpenLoop();
        }
    };

    endpoint_->RegisterMsgHandler(handler);

    // Setup timers
    startTime_ = GetMicrosecondTimestamp();

    uint32_t runtimeSeconds = config.clientRuntimeSeconds;
    terminateTimer_ = std::make_unique<Timer>(
        [runtimeSeconds](void *ctx, void *endpoint) {
            LOG(INFO) << "Exiting after running for " << runtimeSeconds << " seconds";
            exit(0);
        },
        runtimeSeconds * 1000000, this
    );
    ev_set_priority(terminateTimer_->evTimer_, EV_MAXPRI);
    endpoint_->RegisterTimer(terminateTimer_.get());

    if (sendMode_ == flutter::RateBased) {
        // Kick off sending with a small burst every 5 ms
        sendTimer_ = std::make_unique<Timer>([&](void *ctx, void *endpoint) { submitRequestsOpenLoop(); }, 5000, this);
        endpoint_->RegisterTimer(sendTimer_.get());
    }

    // Handle interrupt signals properly on main loop
    endpoint_->RegisterSignalHandler([&]() { endpoint_->LoopBreak(); });

    endpoint_->Connect();

    // Initial sending
    if (sendMode_ == flutter::RateBased) {
        // Send first request immediately
        submitRequest(std::string(requestSize_, 'x'));
    } else if (sendMode_ == flutter::MaxInFlightBased) {
        for (uint32_t i = 0; i < maxInFlight_; i++) {
            submitRequest(std::string(requestSize_, 'x'));
        }
    }

    LOG(INFO) << "Flutter Client " << clientId_ << " initialized with base bet offset " << baseBetOffset_
              << ", bet increment " << betIncrement_ << ", request size " << requestSize_ << " bytes";
}

FlutterClient::~FlutterClient() {}

void FlutterClient::run()
{
    running_ = true;
    LOG(INFO) << "Flutter Client " << clientId_ << " starting event loop";
    endpoint_->LoopRun();
}

void FlutterClient::stop() { endpoint_->LoopBreak(); }

void FlutterClient::submitRequest(const std::string &data)
{
    flutter::proto::FlutterClientRequest request;
    request.set_client_id(clientId_);
    request.set_client_seq(nextSeq_);
    request.set_req_data(data);

    auto state = std::make_unique<FlutterRequestState>(request, GetMicrosecondTimestamp() + baseBetOffset_);

    pendingRequests_[nextSeq_] = std::move(state);
    sendRequest(*pendingRequests_[nextSeq_]);

    nextSeq_++;
    numInFlight_++;
}

void FlutterClient::submitRequestsOpenLoop()
{
    // Don't start rate-based sending until first request is committed
    if (!firstRequestCommitted_) {
        return;
    }

    uint64_t startSendTime = GetMicrosecondTimestamp();
    double sendIntervalUs = 1000000.0 / sendRate_;

    uint64_t numToSend = (startSendTime - lastSendTime_) * sendRate_ / 1000000.0;

    if (numToSend == 0) {
        return;
    }

    // Update lastSendTime accounting for accumulating errors
    lastSendTime_ += numToSend * sendIntervalUs;

    for (uint32_t i = 0; i < numToSend; i++) {
        if (numInFlight_ >= maxInFlight_) {
            break;
        }
        submitRequest(std::string(requestSize_, 'x'));
    }
}

void FlutterClient::sendRequest(FlutterRequestState &state)
{
    // Update bet
    state.bet = GetMicrosecondTimestamp() + baseBetOffset_ + std::pow(2, (state.numRetries)) * betIncrement_;
    state.request.set_bet(state.bet);

    VLOG(1) << "PERF event=send client_id=" << clientId_ << " client_seq=" << nextSeq_ << " bet=" << state.bet
            << " inflight=" << numInFlight_ << " retries=" << state.numRetries;

    // Send FlutterClientRequest directly to all replicas
    sendThreadpool_.enqueueTask([=, this](byte *buffer) {
        for (const auto &addr : replicaAddrs_) {

            MessageHeader *hdr = endpoint_->PrepareProtoMsg(state.request, MessageType::FLUTTER_CLIENT_REQUEST, buffer);

            if (useHMAC_) {
                hmacProvider_.appendMAC(hdr, SEND_BUFFER_SIZE, {NodeType::REPLICA, 0});   // Replica 0 for HMAC
            } else {
                sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
            }

            endpoint_->SendPreparedMsgTo(addr, hdr);
        }

        VLOG(6) << "Sent Flutter request to all replicas: client=" << clientId_ << " seq=" << state.clientSeq
                << " bet=" << state.bet;
    });
}

void FlutterClient::handleMessage(MessageHeader *msgHdr, byte *msgBuffer, Address *sender)
{
    byte *body = (byte *) (msgHdr + 1);

    if (msgHdr->msgType == MessageType::FLUTTER_REPLY) {
        flutter::proto::FlutterReply reply;
        if (!reply.ParseFromArray(body, msgHdr->msgLen)) {
            LOG(ERROR) << "Failed to parse FlutterReply message";
            return;
        }
        processFlutterReply(reply);
    }
}

void FlutterClient::commitRequest(uint32_t clientSeq)
{
    auto it = pendingRequests_.find(clientSeq);
    if (it == pendingRequests_.end()) {
        return;
    }

    FlutterRequestState &state = *it->second;
    state.completed = true;

    pendingRequests_.erase(it);
    numInFlight_--;

    // Enable rate-based sending after first request is committed
    if (!firstRequestCommitted_) {
        firstRequestCommitted_ = true;
        if (sendMode_ == flutter::RateBased) {
            lastSendTime_ = GetMicrosecondTimestamp();
        }
    }

    VLOG(4) << "After commit: inflight=" << numInFlight_;

    if (sendMode_ == flutter::MaxInFlightBased) {
        submitRequest("request_data");
    }
}

void FlutterClient::processFlutterReply(const flutter::proto::FlutterReply &reply)
{
    uint32_t seq = reply.client_seq();

    auto it = pendingRequests_.find(seq);
    if (it == pendingRequests_.end()) {
        VLOG(4) << "Received reply for unknown request seq=" << seq;
        return;
    }

    FlutterRequestState &state = *it->second;
    uint32_t replicaId = reply.replica_id();

    // Check if we already received a vote from this replica
    if (state.votedReplicas.count(replicaId) > 0) {
        VLOG(4) << "Duplicate vote from replica=" << replicaId << " seq=" << seq;
        return;
    }

    // Record the vote
    state.votedReplicas.insert(replicaId);

    if (reply.accepted()) {
        state.acceptVotes++;
        VLOG(3) << "Vote ACCEPT replica=" << replicaId << " seq=" << seq << " (" << state.acceptVotes << "/" << (f_ + 1)
                << ")";

        // Check if we have f+1 accept votes
        if (state.acceptVotes >= f_ + 1) {
            LOG(INFO) << "PERF event=commit client_id=" << clientId_ << " client_seq=" << seq
                      << " latency=" << (GetMicrosecondTimestamp() - state.submitTime)
                      << " retries=" << state.numRetries << " decision=accept path=flutter";

            commitRequest(seq);
        }
    } else {
        state.rejectVotes++;
        VLOG(3) << "Vote REJECT replica=" << replicaId << " seq=" << seq << " (" << state.rejectVotes << "/" << (f_ + 1)
                << ")";

        // Check if we have f+1 reject votes
        if (state.rejectVotes >= f_ + 1) {
            VLOG(1) << "RETRY client=" << clientId_ << " seq=" << seq << " retries=" << (state.numRetries + 1)
                    << " reason=rejected";

            // Reset vote tracking for retry
            state.acceptVotes = 0;
            state.rejectVotes = 0;
            state.votedReplicas.clear();
            state.numRetries++;

            sendRequest(state);
        }
    }
}

}   // namespace flutter