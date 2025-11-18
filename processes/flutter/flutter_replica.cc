#include "flutter_replica.h"

#include "lib/common.h"
#include "lib/config/config_util.h"
#include "lib/transport/nng_endpoint_threaded.h"
#include "lib/transport/ooo_rpc_endpoint.h"
#include "lib/transport/udp_endpoint.h"
#include "proto/flutter_proto.pb.h"

#include <algorithm>
#include <chrono>
#include <cryptopp/sha.h>
#include <sstream>

namespace dombft {
using namespace flutter::proto;

FlutterReplica::FlutterReplica(uint32_t replicaId, uint64_t clockBroadcastInterval)
    : replicaId_(replicaId)
    , numVerifyThreads_(ConfigManager::getInstance().getConfig().replicaNumVerifyThreads)
    , sendThreadpool_(ConfigManager::getInstance().getConfig().replicaNumSendThreads)
    , useHMAC_(ConfigManager::getInstance().getConfig().clientUseHMAC)
    , lockTime_(0)
    , lastClockBroadcast_(0)
    , clockBroadcastInterval_(clockBroadcastInterval)
    , leaderId_(0)   // Simple fixed leader (replica 0)
{
    auto &configManager = ConfigManager::getInstance();
    const auto &config = configManager.getConfig();

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

    // Setup clock broadcast timer
    clockTimer_ = std::make_unique<Timer>(
        [this](void *ctx, void *endpoint) {
            this->broadcastClock();
        },
        clockBroadcastInterval_,   // Already in microseconds
        this
    );
    endpoint_->RegisterTimer(clockTimer_.get());

    endpoint_->Connect();

    // Initialize clock management state
    lockTime_ = 0;
    lastClockBroadcast_ = GetMicrosecondTimestamp();

    // Initialize our own clock in the replica clocks map
    replicaClocks_[replicaId_] = GetMicrosecondTimestamp();

    LOG(INFO) << "Flutter replica " << replicaId_ << " initialized with clock broadcast interval "
              << clockBroadcastInterval_ << "us";
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

    // Check if we need to broadcast our clock
    uint64_t now = GetMicrosecondTimestamp();
    if (now - lastClockBroadcast_ >= clockBroadcastInterval_) {
        broadcastClock();
    }

    VLOG(6) << "Queue sizes: verify=" << verifyQueue_.size_approx() << " process=" << processQueue_.size_approx();
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

        if (hdr->msgType == FLUTTER_CLIENT_REQUEST) {
            FlutterClientRequest request;

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

        else if (hdr->msgType == FLUTTER_REPLICA_MSG) {
            flutter::proto::FlutterMessage flutterMsg;
            if (!flutterMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Failed to parse FlutterMessage";
                continue;
            }

            bool verified = false;
            if (useHMAC_) {
                verified = hmacProvider_.verify(hdr, {NodeType::REPLICA, flutterMsg.sender_id()});
            } else {
                verified = sigProvider_.verify(hdr, {NodeType::REPLICA, flutterMsg.sender_id()});
            }

            if (!verified) {
                LOG(INFO) << "Failed to verify replica signature from " << flutterMsg.sender_id();
                continue;
            }

            processQueue_.enqueue(msg);
        }

        else {
            LOG(ERROR) << "Unknown message type " << (int) hdr->msgType << " for verification";
        }
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

        if (hdr->msgType == FLUTTER_CLIENT_REQUEST) {
            FlutterClientRequest clientRequestMsg;

            if (!clientRequestMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
                continue;
            }

            processClientRequest(clientRequestMsg);
        }

        if (hdr->msgType == FLUTTER_REPLICA_MSG) {
            flutter::proto::FlutterMessage flutterMsg;
            if (!flutterMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Failed to parse FlutterMessage";
                continue;
            }
            processFlutterMessage(flutterMsg);
        }
        // TODO: Add Flutter protocol message handling
    }
}

void FlutterReplica::processClientRequest(const flutter::proto::FlutterClientRequest &request)
{
    uint64_t bet = request.bet();
    std::pair<uint64_t, uint32_t> key = {bet, request.client_id()};
    bool isFirstTime = candidatePool_.find(key) == candidatePool_.end();

    if (isFirstTime) {
        initializeCandidate(request, bet);
    } else {
        VLOG(4) << "Duplicate client request: client=" << request.client_id() << " seq=" << request.client_seq()
                << " bet=" << bet;
    }
}

void FlutterReplica::processFlutterMessage(const flutter::proto::FlutterMessage &msg)
{
    // Handle different types of Flutter protocol messages
    if (msg.has_time()) {
        processFlutterTime(msg.sender_id(), msg.time().local_time());
    } else if (msg.has_observe()) {
        // Handle observe messages (relay client requests)
        flutter::proto::FlutterClientRequest observedRequest;
        if (observedRequest.ParseFromString(msg.observe().message())) {
            processObserve(msg.sender_id(), observedRequest, msg.observe().bet());
        } else {
            LOG(ERROR) << "FLUTTER: Failed to parse client request from observe message";
        }
    } else if (msg.has_rbc_proposal()) {
        // Handle RBC proposal messages
        processRBCProposal(
            msg.sender_id(), msg.rbc_proposal().client_id(), msg.rbc_proposal().bet(), msg.rbc_proposal().accept()
        );
    } else if (msg.has_rbc_slow_proposal()) {
        // Handle RBC slow proposal messages
        processRBCSlowProposal(
            msg.sender_id(), msg.rbc_slow_proposal().client_id(), msg.rbc_slow_proposal().bet(),
            msg.rbc_slow_proposal().accept()
        );
    } else if (msg.has_rbc_slow_value()) {
        // Handle RBC slow value messages
        processRBCSlowValue(
            msg.sender_id(), msg.rbc_slow_value().client_id(), msg.rbc_slow_value().bet(), msg.rbc_slow_value().accept()
        );
    }
}

void FlutterReplica::initializeCandidate(const flutter::proto::FlutterClientRequest &request, uint64_t bet)
{
    uint32_t clientId = request.client_id();
    uint32_t clientSeq = request.client_seq();

    // Check if this request has already been committed
    if (clientSeqTrackers_[clientId].isCommitted(clientSeq)) {
        VLOG(4) << "Ignoring already-committed: client=" << clientId << " seq=" << clientSeq;
        return;
    }

    // Create candidate
    Candidate candidate;
    candidate.clientId = clientId;
    candidate.clientSeq = clientSeq;
    candidate.bet = bet;
    candidate.request = request;

    // Compute digest of the request
    std::string reqSerialized = request.SerializeAsString();
    CryptoPP::SHA256 hash;
    byte digest[CryptoPP::SHA256::DIGESTSIZE];
    hash.CalculateDigest(digest, (const byte *) reqSerialized.c_str(), reqSerialized.size());
    candidate.digest = std::string(reinterpret_cast<const char *>(digest), CryptoPP::SHA256::DIGESTSIZE);

    // Add to candidate pool
    std::pair<uint64_t, uint32_t> key = {bet, request.client_id()};
    candidatePool_[key] = candidate;

    VLOG(1) << "RECV_REQUEST client=" << request.client_id() << " seq=" << request.client_seq() << " bet=" << bet;

    // Broadcast observe message
    broadcastObserve(request, bet);

    // Determine RBC proposal based on current time vs deadline/bet
    uint64_t currentTime = GetMicrosecondTimestamp();
    bool acceptProposal = currentTime <= bet;   // Accept if current time is before or at deadline

    VLOG(2) << "Proposal " << (acceptProposal ? "ACCEPT" : "REJECT") << " client=" << request.client_id() << " bet="
            << bet << " time=" << currentTime;

    // Broadcast RBC proposal
    broadcastRBCProposal(request.client_id(), bet, acceptProposal);
}

void FlutterReplica::broadcastClock()
{
    uint64_t currentTime = GetMicrosecondTimestamp();

    // Check if enough time has elapsed since last broadcast
    if (currentTime - lastClockBroadcast_ < clockBroadcastInterval_) {
        VLOG(5) << "Skipping clock broadcast, only " << (currentTime - lastClockBroadcast_) / 1000 << "ms elapsed";
        return;
    }

    // Update our own clock
    replicaClocks_[replicaId_] = currentTime;

    // Create FlutterTime message
    flutter::proto::FlutterTime timeMsg;
    timeMsg.set_local_time(currentTime);

    // Create container message
    flutter::proto::FlutterMessage flutterMsg;
    flutterMsg.set_sender_id(replicaId_);
    *flutterMsg.mutable_time() = timeMsg;

    // Broadcast to all replicas using FLUTTER_REPLICA_MSG type
    broadcastToReplicas(flutterMsg, MessageType::FLUTTER_REPLICA_MSG);

    lastClockBroadcast_ = currentTime;

    VLOG(6) << "Broadcast clock time=" << currentTime;
}

void FlutterReplica::processFlutterTime(uint32_t senderId, uint64_t clockTime)
{
    // Update the sender's clock time
    replicaClocks_[senderId] = clockTime;

    VLOG(6) << "Recv clock from replica=" << senderId << " time=" << clockTime;

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

        if (lockTime_ != oldLockTime) {
            VLOG(2) << "Lock time updated: " << oldLockTime << " -> " << lockTime_;
        }

        // If lock time advanced, check for candidates that can now be committed
        if (lockTime_ > oldLockTime) {
            checkCandidatesForCommit();
        }
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
    broadcastToReplicas(flutterMsg, MessageType::FLUTTER_REPLICA_MSG);

    VLOG(6) << "Broadcast RBC proposal: client=" << clientId << " bet=" << bet << " vote=" << (accept ? "ACCEPT" : "REJECT");
}

void FlutterReplica::processRBCProposal(uint32_t senderId, uint32_t clientId, uint64_t bet, bool accept)
{
    // Find the candidate in our pool
    std::pair<uint64_t, uint32_t> key = {bet, clientId};
    auto it = candidatePool_.find(key);

    if (it == candidatePool_.end()) {
        LOG(WARNING) << "FLUTTER: Received RBC proposal for unknown candidate client=" << clientId << " bet=" << bet;
        return;
    }

    Candidate &candidate = it->second;

    // Check if this replica has already voted
    if (candidate.votedReplicas.count(senderId) > 0) {
        LOG(INFO) << "FLUTTER: Replica " << senderId << " already voted for client=" << clientId << " bet=" << bet;
        return;
    }

    // Record the vote
    candidate.votedReplicas.insert(senderId);
    if (accept) {
        candidate.acceptVotes++;
    } else {
        candidate.rejectVotes++;
    }

    VLOG(3) << "RBC vote from replica=" << senderId << " client=" << clientId << " bet=" << bet
            << " vote=" << (accept ? "ACCEPT" : "REJECT") << " totals: accept=" << candidate.acceptVotes
            << " reject=" << candidate.rejectVotes;

    // Check for commits only if we just reached superquorum threshold
    if (candidate.acceptVotes == superQuorumSize_ || candidate.rejectVotes == superQuorumSize_) {
        checkCandidatesForCommit();
    }

    // Check if we should initiate slow path: total votes reached superquorum but no single type has superquorum
    uint32_t totalVotes = candidate.acceptVotes + candidate.rejectVotes;
    if (totalVotes == superQuorumSize_ && !candidate.slowPathInitiated && candidate.acceptVotes < superQuorumSize_ &&
        candidate.rejectVotes < superQuorumSize_) {

        candidate.slowPathInitiated = true;

        // Send slow proposal to leader with majority vote
        bool majorityAccept = candidate.acceptVotes > candidate.rejectVotes;
        sendRBCSlowProposal(clientId, bet, majorityAccept);

        VLOG(2) << "SLOW_PATH initiated: client=" << clientId << " bet=" << bet << " accept=" << candidate.acceptVotes
                << " reject=" << candidate.rejectVotes << " majority=" << (majorityAccept ? "ACCEPT" : "REJECT");
    }
}

void FlutterReplica::broadcastObserve(const flutter::proto::FlutterClientRequest &request, uint64_t bet)
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
    broadcastToReplicas(flutterMsg, MessageType::FLUTTER_REPLICA_MSG);

    VLOG(6) << "Broadcast observe: client=" << request.client_id() << " bet=" << bet;
}

void FlutterReplica::processObserve(
    uint32_t senderId, const flutter::proto::FlutterClientRequest &request, uint64_t bet
)
{
    // Check if this is the first time seeing this request
    std::pair<uint64_t, uint32_t> key = {bet, request.client_id()};
    bool isFirstTime = candidatePool_.find(key) == candidatePool_.end();

    if (isFirstTime) {
        VLOG(4) << "Recv observe from replica=" << senderId << " client=" << request.client_id() << " bet=" << bet;

        // Use the same initialization logic as for direct client requests
        initializeCandidate(request, bet);
    } else {
        VLOG(5) << "Duplicate observe from replica=" << senderId << " client=" << request.client_id() << " bet=" << bet;
    }
}

void FlutterReplica::checkCandidatesForCommit()
{
    std::vector<std::pair<uint64_t, uint32_t>> candidatesToRemove;

    for (auto &[key, candidate] : candidatePool_) {
        uint64_t bet = candidate.bet;
        uint32_t clientId = candidate.clientId;

        // Check if candidate has converged (lock time > bet)
        bool hasConverged = lockTime_ > bet;

        // Early exit: since map is ordered by timestamp, if this candidate hasn't converged,
        // all subsequent candidates have higher timestamps and also won't have converged
        if (!hasConverged) {
            break;
        }

        // Check if we have consensus either from fast path (superquorum votes) or slow path (leader decision)
        bool hasFastConsensus = candidate.acceptVotes >= superQuorumSize_ || candidate.rejectVotes >= superQuorumSize_;
        bool hasSlowConsensus = candidate.slowDecision != false;   // slowDecision is set when leader broadcasts

        if (hasFastConsensus || hasSlowConsensus) {
            bool accepted;
            if (hasFastConsensus) {
                accepted = candidate.acceptVotes >= superQuorumSize_;
            } else {
                accepted = candidate.slowDecision;
            }

            VLOG(1) << "COMMIT client=" << clientId << " seq=" << candidate.clientSeq << " bet=" << bet << " decision="
                    << (accepted ? "ACCEPT" : "REJECT") << " path="
                    << (hasFastConsensus ? "fast" : "slow") << " lock=" << lockTime_;

            // Mark sequence as committed to prevent reprocessing
            clientSeqTrackers_[clientId].commit(candidate.clientSeq);
            VLOG(4) << "Marked committed: client=" << clientId << " seq=" << candidate.clientSeq;

            if (accepted) {
                // TODO: Execute the request
                VLOG(2) << "Execute request: client=" << clientId << " seq=" << candidate.clientSeq;

                // Send reply to client
                flutter::proto::FlutterReply reply;
                reply.set_replica_id(replicaId_);
                reply.set_client_id(clientId);
                reply.set_client_seq(candidate.clientSeq);
                reply.set_bet(candidate.bet);
                reply.set_accepted(accepted);
                reply.set_result("Request executed successfully");
                sendMsgToDst(reply, MessageType::FLUTTER_REPLY, clientAddrs_[clientId]);

                VLOG(6) << "Sent reply to client=" << clientId << " seq=" << candidate.clientSeq;
            } else {
                VLOG(2) << "Reject (not executing): client=" << clientId << " seq=" << candidate.clientSeq;
            }

            // Mark for removal from candidate pool
            candidatesToRemove.push_back(key);
        }
    }

    // Remove committed candidates from the pool
    for (const auto &key : candidatesToRemove) {
        candidatePool_.erase(key);
    }

    if (!candidatesToRemove.empty()) {
        VLOG(3) << "Removed " << candidatesToRemove.size() << " committed candidates from pool";
    }
}

void FlutterReplica::sendRBCSlowProposal(uint32_t clientId, uint64_t bet, bool accept)
{
    flutter::proto::FlutterRBCSlowProposal slowProposal;
    slowProposal.set_client_id(clientId);
    slowProposal.set_bet(bet);
    slowProposal.set_accept(accept);

    flutter::proto::FlutterMessage flutterMsg;
    flutterMsg.set_sender_id(replicaId_);
    *flutterMsg.mutable_rbc_slow_proposal() = slowProposal;

    sendMsgToDst(flutterMsg, MessageType::FLUTTER_REPLICA_MSG, replicaAddrs_[leaderId_]);

    VLOG(3) << "Sent slow proposal to leader=" << leaderId_ << " client=" << clientId << " bet=" << bet
            << " vote=" << (accept ? "ACCEPT" : "REJECT");
}

void FlutterReplica::processRBCSlowProposal(uint32_t senderId, uint32_t clientId, uint64_t bet, bool accept)
{
    // Only leader processes slow proposals
    if (replicaId_ != leaderId_) {
        LOG(WARNING) << "FLUTTER: Non-leader replica " << replicaId_ << " received RBC slow proposal";
        return;
    }

    std::pair<uint64_t, uint32_t> key = {bet, clientId};
    auto it = candidatePool_.find(key);
    if (it == candidatePool_.end()) {
        LOG(WARNING) << "FLUTTER: Leader received slow proposal for unknown candidate "
                     << "client " << clientId << " bet " << bet;
        return;
    }

    Candidate &candidate = it->second;

    // Record the slow proposal
    candidate.slowProposals[senderId] = accept;

    VLOG(3) << "Leader recv slow proposal from replica=" << senderId << " client=" << clientId << " bet=" << bet
            << " vote=" << (accept ? "ACCEPT" : "REJECT") << " total=" << candidate.slowProposals.size() << "/"
            << (f_ + 1);

    // Count accept and reject votes
    uint32_t acceptCount = 0;
    uint32_t rejectCount = 0;
    for (const auto &[replicaId, vote] : candidate.slowProposals) {
        if (vote) {
            acceptCount++;
        } else {
            rejectCount++;
        }
    }

    // Decide based on first threshold reached
    // If 4f + 1 fast accepts were received by a replica, no correct replica would see a majority of
    // rejects, so leader only needs f + 1 slow votes to make a decision
    bool decision;
    if (acceptCount >= f_ + 1) {
        decision = true;
    } else if (rejectCount >= f_ + 1) {
        decision = false;
    } else {
        // Not enough votes yet
        return;
    }

    // Broadcast slow value decision to all replicas
    sendRBCSlowValue(clientId, bet, decision);

    VLOG(2) << "Leader slow decision: client=" << clientId << " bet=" << bet << " decision="
            << (decision ? "ACCEPT" : "REJECT") << " (accept=" << acceptCount << " reject=" << rejectCount << ")";
}

void FlutterReplica::sendRBCSlowValue(uint32_t clientId, uint64_t bet, bool accept)
{
    flutter::proto::FlutterRBCSlowValue slowValue;
    slowValue.set_client_id(clientId);
    slowValue.set_bet(bet);
    slowValue.set_accept(accept);

    flutter::proto::FlutterMessage flutterMsg;
    flutterMsg.set_sender_id(replicaId_);
    *flutterMsg.mutable_rbc_slow_value() = slowValue;

    broadcastToReplicas(flutterMsg, MessageType::FLUTTER_REPLICA_MSG);

    VLOG(3) << "Leader broadcast slow value: client=" << clientId << " bet=" << bet << " decision="
            << (accept ? "ACCEPT" : "REJECT");
}

void FlutterReplica::processRBCSlowValue(uint32_t senderId, uint32_t clientId, uint64_t bet, bool accept)
{
    // Only accept slow values from the leader
    if (senderId != leaderId_) {
        LOG(WARNING) << "FLUTTER: Received RBC slow value from non-leader replica " << senderId;
        return;
    }

    std::pair<uint64_t, uint32_t> key = {bet, clientId};
    auto it = candidatePool_.find(key);
    if (it == candidatePool_.end()) {
        LOG(WARNING) << "FLUTTER: Received slow value for unknown candidate "
                     << "client " << clientId << " bet " << bet;
        return;
    }

    Candidate &candidate = it->second;
    candidate.slowDecision = accept;

    VLOG(3) << "Recv slow value from leader: client=" << clientId << " bet=" << bet << " decision="
            << (accept ? "ACCEPT" : "REJECT");
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

}   // namespace dombft