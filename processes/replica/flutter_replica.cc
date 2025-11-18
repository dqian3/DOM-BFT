#include "flutter_replica.h"

#include "lib/common.h"
#include "lib/config/config_util.h"
#include "lib/transport/nng_endpoint_threaded.h"
#include "lib/transport/ooo_rpc_endpoint.h"
#include "lib/transport/udp_endpoint.h"
#include "proto/flutter_proto.pb.h"

#include <algorithm>
#include <chrono>
#include <openssl/sha.h>
#include <sstream>

namespace dombft {
using namespace dombft::proto;

FlutterReplica::FlutterReplica(uint32_t replicaId, uint32_t batchSize)
    : replicaId_(replicaId)
    , batchSize_(batchSize)
    , numVerifyThreads_(ConfigManager::getInstance().getConfig().replicaNumVerifyThreads)
    , sendThreadpool_(ConfigManager::getInstance().getConfig().replicaNumSendThreads)
    , useHMAC_(ConfigManager::getInstance().getConfig().clientUseHMAC)
    , lockTime_(0)
    , lastClockBroadcast_(0)
{
    auto &configManager = ConfigManager::getInstance();
    const auto &config = configManager.getConfig();

    LOG(INFO) << "Flutter Replica batchSize=" << batchSize_;

    const auto &replicaIps = configManager.getReplicaIps();
    std::string replicaIp = replicaIps[replicaId];
    LOG(INFO) << "replicaIP=" << replicaIp;

    std::string bindAddress = replicaIp;
    LOG(INFO) << "bindAddress=" << bindAddress;

    int replicaPort = configManager.getReplicaPort();
    LOG(INFO) << "replicaPort=" << replicaPort;

    // Load cryptographic keys
    std::string replicaKey = config.replicaKeysDir + "/replica" + std::to_string(replicaId_) + ".der";
    LOG(INFO) << "Loading key from " << replicaKey;
    if (!sigProvider_.loadPrivateKey(replicaKey)) {
        LOG(ERROR) << "Unable to load private key!";
        exit(1);
    }

    if (!sigProvider_.loadPublicKeys(NodeType::CLIENT, config.clientKeysDir)) {
        LOG(ERROR) << "Unable to load client public keys!";
        exit(1);
    }

    if (!sigProvider_.loadPublicKeys(NodeType::REPLICA, config.replicaKeysDir)) {
        LOG(ERROR) << "Unable to load replica public keys!";
        exit(1);
    }

    hmacProvider_.loadReplicaKeysDev({NodeType::REPLICA, replicaId_}, configManager.getNumClients());

    // Calculate BFT parameters - Flutter requires 5f + 1 replicas
    numReplicas_ = configManager.getNumReplicas();
    f_ = (numReplicas_ - 1) / 5;   // For 5f + 1 requirement
    quorumSize_ = 3 * f_ + 1;
    superQuorumSize_ = 4 * f_ + 1;

    LOG(INFO) << "Flutter BFT parameters: n=" << numReplicas_ << " f=" << f_ << " quorum=" << quorumSize_
              << " superQuorum=" << superQuorumSize_;

    // Setup network addresses
    if (config.transport == "nng") {
        auto addrPairs = getReplicaAddrs(config, replicaId_);

        size_t nClients = configManager.getNumClients();
        size_t nProxies = configManager.getNumProxies();

        // Client addresses
        for (size_t i = 0; i < nClients; i++) {
            clientAddrs_.push_back(addrPairs[i].second);
        }

        // Proxy addresses
        for (size_t i = nClients; i < nClients + nProxies; i++) {
            proxyAddrs_.push_back(addrPairs[i].second);
        }

        // Replica addresses
        for (size_t i = nClients + nProxies; i < addrPairs.size(); i++) {
            replicaAddrs_.push_back(addrPairs[i].second);
        }

        endpoint_ = std::make_unique<NngEndpointThreaded>(addrPairs, true, Address(replicaIp, replicaPort));
    } else if (config.transport == "udp") {
        size_t nClients = configManager.getNumClients();
        const auto &clientIps = configManager.getClientIps();
        for (size_t i = 0; i < nClients; i++) {
            clientAddrs_.push_back(Address(clientIps[i], configManager.getClientPort() + i));
        }

        const auto &replicaIps = configManager.getReplicaIps();
        for (size_t i = 0; i < replicaIps.size(); i++) {
            if (i != replicaId_) {
                replicaAddrs_.push_back(Address(replicaIps[i], configManager.getReplicaPort()));
            }
        }

        endpoint_ = std::make_unique<UDPEndpoint>(bindAddress, replicaPort);
    } else if (config.transport == "simple-rpc") {
        std::vector<Address> allAddrs;

        size_t nClients = configManager.getNumClients();
        const auto &clientIps = configManager.getClientIps();
        for (int i = 0; i < clientIps.size(); i++) {
            clientAddrs_.push_back(Address(clientIps[i], configManager.getClientPort() + i));
            allAddrs.push_back(clientAddrs_.back());
        }

        const auto &replicaIps = configManager.getReplicaIps();
        for (int i = 0; i < replicaIps.size(); i++) {
            replicaAddrs_.push_back(Address(replicaIps[i], configManager.getReplicaPort()));
            allAddrs.push_back(replicaAddrs_.back());
        }

        endpoint_ = std::make_unique<OOORPCEndpoint>(bindAddress, replicaPort, allAddrs, sendThreadpool_.size());
    } else {
        LOG(ERROR) << "Unsupported transport " << config.transport;
    }

    // Setup message handler
    MessageHandlerFunc handler = [this](MessageHeader *msgHdr, byte *msgBuffer, Address *sender) {
        this->handleMessage(msgHdr, msgBuffer, sender);
    };

    endpoint_->RegisterMsgHandler(handler);

    endpoint_->RegisterSignalHandler([&]() {
        LOG(INFO) << "Received interrupt signal!";
        running_ = false;
        endpoint_->LoopBreak();
    });

    endpoint_->Connect();

    // Initialize clock management state
    lockTime_ = 0;
    lastClockBroadcast_ = GetMicrosecondTimestamp();

    // Initialize our own clock in the replica clocks map
    replicaClocks_[replicaId_] = GetMicrosecondTimestamp();

    roundStartTime_ = std::chrono::steady_clock::now();
}

FlutterReplica::~FlutterReplica()
{
    // Cleanup handled by smart pointers and RAII
}

void FlutterReplica::run()
{
    LOG(INFO) << "Starting " << numVerifyThreads_ << " verify threads";
    running_ = true;
    for (int i = 0; i < numVerifyThreads_; i++) {
        verifyThreads_.emplace_back(&FlutterReplica::verifyMessagesThd, this);
    }

    LOG(INFO) << "Starting process thread";
    processThread_ = std::thread(&FlutterReplica::processMessagesThd, this);

    LOG(INFO) << "Starting main event loop...";
    endpoint_->LoopRun();
    LOG(INFO) << "Finishing main event loop...";

    for (std::thread &thd : verifyThreads_) {
        thd.join();
    }
    processThread_.join();
}

void FlutterReplica::handleMessage(MessageHeader *msgHdr, byte *msgBuffer, Address *sender)
{
    std::vector<byte> msg((byte *) msgHdr, (byte *) msgHdr + sizeof(MessageHeader) + msgHdr->msgLen + msgHdr->sigLen);

    // Skip verification of our own messages
    if (*sender == replicaAddrs_[replicaId_]) {
        processQueue_.enqueue(msg);
    } else {
        verifyQueue_.enqueue(msg);
    }

    VLOG(6) << verifyQueue_.size_approx() << " messages in verify queue, " << processQueue_.size_approx()
            << " messages in process queue";
}

void FlutterReplica::verifyMessagesThd()
{
    std::vector<byte> msg;

    while (running_) {
        if (!verifyQueue_.wait_dequeue_timed(msg, 50000)) {
            continue;
        }

        MessageHeader *hdr = (MessageHeader *) msg.data();
        byte *body = (byte *) (hdr + 1);

        if (hdr->msgType == CLIENT_REQUEST) {
            ClientRequest request;

            if (!request.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
                continue;
            }

            bool verified = false;
            if (useHMAC_) {
                verified = hmacProvider_.verify(hdr, {NodeType::CLIENT, request.client_id()});
            } else {
                verified = sigProvider_.verify(hdr, {NodeType::CLIENT, request.client_id()});
            }

            if (!verified) {
                LOG(INFO) << "Failed to verify client signature from " << request.client_id();
                continue;
            }

            processQueue_.enqueue(msg);
        }
        // TODO: Add Flutter protocol message verification
    }
}

void FlutterReplica::processMessagesThd()
{
    std::vector<byte> msg;

    while (running_) {
        if (!processQueue_.wait_dequeue_timed(msg, 50000)) {
            continue;
        }

        MessageHeader *hdr = (MessageHeader *) msg.data();
        byte *body = (byte *) (hdr + 1);

        if (hdr->msgType == CLIENT_REQUEST) {
            ClientRequest clientRequestMsg;

            if (!clientRequestMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
                continue;
            }

            processClientRequest(clientRequestMsg, std::span{body + hdr->msgLen, hdr->sigLen});
        }
        // TODO: Add Flutter protocol message handling
    }
}

void FlutterReplica::processClientRequest(const dombft::proto::ClientRequest &request, std::span<byte> sig)
{
    uint64_t bet = request.deadline();
    std::pair<uint64_t, uint32_t> key = {bet, request.client_id()};
    bool isFirstTime = candidatePool_.find(key) == candidatePool_.end();

    if (isFirstTime) {
        initializeCandidate(request, bet);
    } else {
        LOG(INFO) << "FLUTTER: Received request from client " << request.client_id() << " seq " << request.client_seq()
                  << " with timestamp " << bet << ", already in candidate pool";
    }
}

void FlutterReplica::initializeCandidate(const dombft::proto::ClientRequest& request, uint64_t bet)
{
    // Create candidate
    Candidate candidate;
    candidate.clientId = request.client_id();
    candidate.clientSeq = request.client_seq();
    candidate.timestamp = bet;
    candidate.request = request;

    // Compute digest of the request
    std::string reqSerialized = request.SerializeAsString();
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, reqSerialized.c_str(), reqSerialized.size());
    SHA256_Final(hash, &sha256);
    candidate.digest = std::string((char*)hash, SHA256_DIGEST_LENGTH);

    // Add to candidate pool
    std::pair<uint64_t, uint32_t> key = {bet, request.client_id()};
    candidatePool_[key] = candidate;

    LOG(INFO) << "FLUTTER: Initialized candidate for client " << request.client_id()
              << " seq " << request.client_seq() << " with bet " << bet;

    // Broadcast observe message
    broadcastObserve(request, bet);

    // Determine RBC proposal based on current time vs deadline/bet
    uint64_t currentTime = GetMicrosecondTimestamp();
    bool acceptProposal = currentTime <= bet;  // Accept if current time is before or at deadline

    LOG(INFO) << "FLUTTER: Proposing " << (acceptProposal ? "ACCEPT" : "REJECT")
              << " for client " << request.client_id() << " (current: " << currentTime
              << ", deadline: " << bet << ")";

    // Broadcast RBC proposal
    broadcastRBCProposal(request.client_id(), bet, acceptProposal);
}

void FlutterReplica::broadcastClock()
{
    uint64_t currentTime = GetMicrosecondTimestamp();

    // Update our own clock
    replicaClocks_[replicaId_] = currentTime;

    // Create FlutterTime message
    flutter::proto::FlutterTime timeMsg;
    timeMsg.set_local_time(currentTime);

    // Create container message
    flutter::proto::FlutterMessage flutterMsg;
    flutterMsg.set_sender_id(replicaId_);
    *flutterMsg.mutable_time() = timeMsg;

    // Broadcast to all replicas using DUMMY_PROTO type
    broadcastToReplicas(flutterMsg, MessageType::DUMMY_PROTO);

    lastClockBroadcast_ = currentTime;

    LOG(INFO) << "FLUTTER: Broadcasted clock time " << currentTime;
}

void FlutterReplica::processFlutterTime(uint32_t senderId, uint64_t clockTime)
{
    // Update the sender's clock time
    replicaClocks_[senderId] = clockTime;

    LOG(INFO) << "FLUTTER: Received clock time " << clockTime << " from replica " << senderId;

    // Update lock time with new clock data
    updateLockTime();
}

void FlutterReplica::updateLockTime()
{
    if (replicaClocks_.size() < superQuorumSize_) {
        return;   // Need at least 4f+1 clocks
    }

    std::vector<uint64_t> clockTimes;
    for (const auto &pair : replicaClocks_) {
        clockTimes.push_back(pair.second);
    }

    // Sort to find the 4f+1th lowest time
    std::sort(clockTimes.begin(), clockTimes.end());

    if (clockTimes.size() >= superQuorumSize_) {
        uint64_t oldLockTime = lockTime_;
        lockTime_ = clockTimes[superQuorumSize_ - 1];   // 4f+1th lowest (0-indexed)
        LOG(INFO) << "FLUTTER: Updated lock time to " << lockTime_;

        // If lock time advanced, check for candidates that can now be committed
        if (lockTime_ > oldLockTime) {
            checkCandidatesForCommit();
        }
    }
}

void FlutterReplica::processFlutterMessage(const flutter::proto::FlutterMessage &msg)
{
    // Handle different types of Flutter protocol messages
    if (msg.has_time()) {
        processFlutterTime(msg.sender_id(), msg.time().local_time());
    } else if (msg.has_observe()) {
        // Handle observe messages (relay client requests)
        dombft::proto::ClientRequest observedRequest;
        if (observedRequest.ParseFromString(msg.observe().message())) {
            processObserve(msg.sender_id(), observedRequest, msg.observe().bet());
        } else {
            LOG(ERROR) << "FLUTTER: Failed to parse client request from observe message";
        }
    } else if (msg.has_rbc_proposal()) {
        // Handle RBC proposal messages
        LOG(INFO) << "FLUTTER: Received RBC proposal for client " << msg.rbc_proposal().client_id() << " bet "
                  << msg.rbc_proposal().bet() << " accept=" << msg.rbc_proposal().accept();
        // TODO: Process RBC proposal
        // Handle RBC proposal messages
        processRBCProposal(msg.sender_id(), msg.rbc_proposal().client_id(), msg.rbc_proposal().bet(), msg.rbc_proposal().accept());
    }
}

void FlutterReplica::broadcastRBCProposal(uint32_t clientId, uint64_t bet, bool accept)
{
    // Create RBC proposal message
    flutter::proto::FlutterRBCProposal rbcProposal;
    rbcProposal.set_client_id(clientId);
    rbcProposal.set_bet(bet);
    rbcProposal.set_accept(accept);

    // Create container message
    flutter::proto::FlutterMessage flutterMsg;
    flutterMsg.set_sender_id(replicaId_);
    *flutterMsg.mutable_rbc_proposal() = rbcProposal;

    // Broadcast to all replicas
    broadcastToReplicas(flutterMsg, MessageType::DUMMY_PROTO);

    LOG(INFO) << "FLUTTER: Broadcasted RBC proposal for client " << clientId << " bet " << bet
              << " accept=" << accept;
}

void FlutterReplica::processRBCProposal(uint32_t senderId, uint32_t clientId, uint64_t bet, bool accept)
{
    // Find the candidate in our pool
    std::pair<uint64_t, uint32_t> key = {bet, clientId};
    auto it = candidatePool_.find(key);

    if (it == candidatePool_.end()) {
        LOG(WARNING) << "FLUTTER: Received RBC proposal for unknown candidate client=" << clientId
                     << " bet=" << bet;
        return;
    }

    Candidate& candidate = it->second;

    // Check if this replica has already voted
    if (candidate.votedReplicas.count(senderId) > 0) {
        LOG(INFO) << "FLUTTER: Replica " << senderId << " already voted for client=" << clientId
                  << " bet=" << bet;
        return;
    }

    // Record the vote
    candidate.votedReplicas.insert(senderId);
    if (accept) {
        candidate.acceptVotes++;
    } else {
        candidate.rejectVotes++;
    }

    LOG(INFO) << "FLUTTER: Processed RBC proposal from replica " << senderId << " for client " << clientId
              << " bet " << bet << " accept=" << accept << ". Total votes: accept=" << candidate.acceptVotes
              << " reject=" << candidate.rejectVotes;

    // Check for commits only if we just reached superquorum threshold
    if (candidate.acceptVotes == superQuorumSize_ || candidate.rejectVotes == superQuorumSize_) {
        checkCandidatesForCommit();
    }
}

void FlutterReplica::broadcastObserve(const dombft::proto::ClientRequest& request, uint64_t bet)
{
    // Serialize the client request
    std::string serializedRequest = request.SerializeAsString();

    // Create observe message
    flutter::proto::FlutterObserve observeMsg;
    observeMsg.set_client_id(request.client_id());
    observeMsg.set_message(serializedRequest);
    observeMsg.set_bet(bet);

    // Create container message
    flutter::proto::FlutterMessage flutterMsg;
    flutterMsg.set_sender_id(replicaId_);
    *flutterMsg.mutable_observe() = observeMsg;

    // Broadcast to all replicas
    broadcastToReplicas(flutterMsg, MessageType::DUMMY_PROTO);

    LOG(INFO) << "FLUTTER: Broadcasted observe message for client " << request.client_id()
              << " with bet " << bet;
}

void FlutterReplica::processObserve(uint32_t senderId, const dombft::proto::ClientRequest& request, uint64_t bet)
{
    // Check if this is the first time seeing this request
    std::pair<uint64_t, uint32_t> key = {bet, request.client_id()};
    bool isFirstTime = candidatePool_.find(key) == candidatePool_.end();

    if (isFirstTime) {
        LOG(INFO) << "FLUTTER: Processed observe from replica " << senderId << " for client "
                  << request.client_id() << " bet " << bet;

        // Use the same initialization logic as for direct client requests
        initializeCandidate(request, bet);
    } else {
        LOG(INFO) << "FLUTTER: Received observe from replica " << senderId << " for already known client "
                  << request.client_id() << " bet " << bet;
    }
}

void FlutterReplica::checkCandidatesForCommit()
{
    std::vector<std::pair<uint64_t, uint32_t>> candidatesToRemove;

    for (auto& [key, candidate] : candidatePool_) {
        uint64_t bet = candidate.timestamp;
        uint32_t clientId = candidate.clientId;

        // Check if candidate has converged (lock time > bet)
        bool hasConverged = lockTime_ > bet;

        // Early exit: since map is ordered by timestamp, if this candidate hasn't converged,
        // all subsequent candidates have higher timestamps and also won't have converged
        if (!hasConverged) {
            break;
        }

        // Check if we have superquorum votes for either accept or reject
        bool hasAcceptConsensus = candidate.acceptVotes >= superQuorumSize_;
        bool hasRejectConsensus = candidate.rejectVotes >= superQuorumSize_;

        if (hasAcceptConsensus || hasRejectConsensus) {
            bool accepted = hasAcceptConsensus;

            LOG(INFO) << "FLUTTER: Candidate ready for commit - client " << clientId
                      << " bet " << bet << " " << (accepted ? "ACCEPTED" : "REJECTED")
                      << " (accept votes: " << candidate.acceptVotes
                      << ", reject votes: " << candidate.rejectVotes
                      << ", lock time: " << lockTime_ << ")";

            if (accepted) {
                // TODO: Execute the request and send reply to client
                LOG(INFO) << "FLUTTER: Executing request for client " << clientId
                          << " seq " << candidate.clientSeq;

                // Send reply to client
                Reply reply;
                reply.set_replica_id(replicaId_);
                reply.set_client_id(clientId);
                reply.set_client_seq(candidate.clientSeq);
                reply.set_round(0);  // TODO: Use actual round if needed
                reply.set_seq(0);    // TODO: Use actual sequence if needed
                reply.set_digest(candidate.digest);

                sendMsgToDst(reply, MessageType::REPLY, clientAddrs_[clientId]);

                LOG(INFO) << "FLUTTER: Sent reply to client " << clientId;
            } else {
                LOG(INFO) << "FLUTTER: Request rejected for client " << clientId
                          << " - not executing";
            }

            // Mark for removal from candidate pool
            candidatesToRemove.push_back(key);
        }
    }

    // Remove committed candidates from the pool
    for (const auto& key : candidatesToRemove) {
        candidatePool_.erase(key);
    }

    if (!candidatesToRemove.empty()) {
        LOG(INFO) << "FLUTTER: Removed " << candidatesToRemove.size()
                  << " committed candidates from pool";
    }
}


// Template instantiations for sending helpers
template <typename T> void FlutterReplica::sendMsgToDst(const T &msg, MessageType type, const Address &dst)
{
    sendThreadpool_.enqueueTask([=, this](byte *buffer) {
        MessageHeader *hdr = endpoint_->PrepareProtoMsg(msg, type, buffer);
        if (useHMAC_ && type == REPLY) {
            auto it = find(clientAddrs_.begin(), clientAddrs_.end(), dst);
            assert(it != clientAddrs_.end());

            uint32_t clientId = it - clientAddrs_.begin();
            hmacProvider_.appendMAC(hdr, SEND_BUFFER_SIZE, {NodeType::CLIENT, clientId});
        } else {
            sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
        }
        endpoint_->SendPreparedMsgTo(dst, hdr);
    });
}

template <typename T> void FlutterReplica::broadcastToReplicas(const T &msg, MessageType type)
{
    sendThreadpool_.enqueueTask([=, this](byte *buffer) {
        MessageHeader *hdr = endpoint_->PrepareProtoMsg(msg, type, buffer);
        sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);

        for (const Address &addr : replicaAddrs_) {
            endpoint_->SendPreparedMsgTo(addr, hdr);
        }
    });
}

}