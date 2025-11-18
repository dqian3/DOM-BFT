#include "flutter_client.h"

#include "lib/config/config_util.h"
#include "lib/transport/nng_endpoint_threaded.h"
#include "lib/transport/ooo_rpc_endpoint.h"
#include "lib/transport/udp_endpoint.h"

#include <chrono>

namespace dombft {

FlutterClient::FlutterClient(uint32_t clientId, uint64_t baseBetOffset, uint64_t betIncrement)
    : clientId_(clientId)
    , sendThreadpool_(4)   // 4 threads for sending
    , baseBetOffset_(baseBetOffset)
    , betIncrement_(betIncrement)
    , running_(false)
{
    auto &configManager = ConfigManager::getInstance();
    const auto &config = configManager.getConfig();

    useHMAC_ = config.clientUseHMAC;

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
        };

    endpoint_->RegisterMsgHandler(handler);
    endpoint_->Connect();

    LOG(INFO) << "Flutter Client " << clientId_ << " initialized with base bet offset " << baseBetOffset_
              << " and increment " << betIncrement_;
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

    LOG(INFO) << "Flutter Client " << clientId_ << " submitting request seq " << nextSeq_ << " with bet " << state->bet;

    pendingRequests_[nextSeq_] = std::move(state);
    // TODO this is questionable in terms of memory safety
    sendRequest(*state);

    nextSeq_++;
}

void FlutterClient::sendRequest(FlutterRequestState &state)
{
    // Update bet and send time
    state.bet = GetMicrosecondTimestamp() + (state.numRetries) * betIncrement_;
    state.request.set_bet(state.bet);

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

        LOG(INFO) << "Flutter Client " << clientId_ << " sent request seq " << state.clientSeq << " with bet "
                  << state.bet;
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

void FlutterClient::processFlutterReply(const flutter::proto::FlutterReply &reply)
{
    uint32_t seq = reply.client_seq();

    auto it = pendingRequests_.find(seq);
    if (it == pendingRequests_.end()) {
        LOG(WARNING) << "Received reply for unknown request seq " << seq;
        return;
    }

    FlutterRequestState &state = *it->second;

    if (reply.accepted()) {
        LOG(INFO) << "Flutter Client " << clientId_ << " request seq " << seq << " ACCEPTED with bet " << reply.bet();

        LOG(INFO) << "PERF" << " event=flutter_commit"
                  << " clientId=" << clientId_ << " clientSeq=" << seq
                  << " latency=" << (GetMicrosecondTimestamp() - state.sendTime) << " numRetries=" << state.numRetries;
        state.completed = true;
        pendingRequests_.erase(it);

    } else {
        LOG(INFO) << "Flutter Client " << clientId_ << " request seq " << seq << " REJECTED with bet " << reply.bet()
                  << " - retrying";

        // TODO this is questionable in terms of memory safety
        state.numRetries++;

        sendRequest(state);
    }
}

}   // namespace dombft