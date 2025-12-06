#include "replica.h"

#include "lib/application.h"
#include "lib/apps/counter.h"
#include "lib/apps/kv_store.h"
#include "lib/common.h"
#include "lib/config/config_util.h"
#include "lib/transport/nng_endpoint_threaded.h"
#include "lib/transport/ooo_rpc_endpoint.h"
#include "lib/transport/udp_endpoint.h"

#include <algorithm>
#include <cryptopp/sha.h>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace dombft {
using namespace dombft::proto;

Replica::Replica(
    uint32_t replicaId, bool crashed, uint32_t swapFreq, uint32_t viewChangeFreq, bool commitLocalInViewChange,
    uint32_t viewChangeNum, uint32_t checkpointDropFreq, bool skipForwarding, bool ignoreDeadlines
)
    : replicaId_(replicaId)
    , checkpointInterval_(ConfigManager::getInstance().getConfig().replicaCheckpointInterval)
    , snapshotInterval_(ConfigManager::getInstance().getConfig().replicaSnapshotInterval)
    , numVerifyThreads_(ConfigManager::getInstance().getConfig().replicaNumVerifyThreads)
    , useHMAC_(ConfigManager::getInstance().getConfig().clientUseHMAC)
    , repairTimeout_(ConfigManager::getInstance().getConfig().replicaRepairTimeout)
    , repairViewTimeout_(ConfigManager::getInstance().getConfig().replicaRepairViewTimeout)
    , proxyPort_(ConfigManager::getInstance().getProxyForwardPort())
    , numReceivers_(ConfigManager::getInstance().getNumReplicas())
    , skipForwarding_(skipForwarding)
    , ignoreDeadlines_(ignoreDeadlines)
    , sigProvider_()
    , sendThreadpool_(ConfigManager::getInstance().getConfig().replicaNumSendThreads)
    , running_(true)
    , round_(1)
    , checkpointCollectors_(replicaId_)
    , crashed_(crashed)
    , swapFreq_(swapFreq)
    , checkpointDropFreq_(checkpointDropFreq)
    , viewChangeFreq_(viewChangeFreq)
    , viewChangeRound_(viewChangeFreq_)
    , commitLocalInViewChange_(commitLocalInViewChange)
    , viewChangeNum_(viewChangeNum)
{
    const auto &config = ConfigManager::getInstance().getConfig();

    // Replica initialization
    std::string replicaIp = config.replicaIps[replicaId];
    LOG(INFO) << "replicaIP=" << replicaIp;

    std::string bindAddress = replicaIp;
    LOG(INFO) << "bindAddress=" << bindAddress;

    int replicaPort = config.replicaPort;
    LOG(INFO) << "replicaPort=" << replicaPort;

    std::string replicaKey = config.replicaKeysDir + "/replica" + std::to_string(replicaId_) + ".der";
    LOG(INFO) << "Loading replica key from " << replicaKey;
    if (!sigProvider_.loadPrivateKey(replicaKey)) {
        LOG(ERROR) << "Unable to load replica private key!";
        exit(1);
    }

    // For unified process, we use the replica key for both replica and receiver functions

    LOG(INFO) << "Private keys loaded";

    if (!sigProvider_.loadPublicKeys(NodeType::CLIENT, config.clientKeysDir)) {
        LOG(ERROR) << "Unable to load client public keys!";
        exit(1);
    }

    if (!sigProvider_.loadPublicKeys(NodeType::REPLICA, config.replicaKeysDir)) {
        LOG(ERROR) << "Unable to load replica public keys!";
        exit(1);
    }

    hmacProvider_.loadReplicaKeysDev(
        {NodeType::REPLICA, replicaId_}, config.clientIps.size(), config.replicaIps.size()
    );

    LOG(INFO) << "Instantiating log and application";

    if (config.app == AppType::COUNTER) {
        app_ = std::make_shared<Counter>();
    } else if (config.app == AppType::KV_STORE) {
        app_ = std::make_shared<KVStore>();
    } else {
        LOG(ERROR) << "Unrecognized App Type";
        exit(1);
    }
    log_ = std::make_shared<Log>(app_);
    LOG(INFO) << "Log instantiated";

    f_ = config.f;
    quorumSize_ = ConfigManager::getInstance().getQuorumSize();
    superQuorumSize_ = ConfigManager::getInstance().getSuperQuorumSize();

    preserializationMode_ = config.preserializationMode;
    if (preserializationMode_ != "disabled") {
        LOG(INFO) << "Preserialization mode: " << preserializationMode_;
    }

    // Network setup for unified functionality
    if (config.transport == "nng") {
        // Use replica addressing for unified process
        auto addrPairs = getReplicaAddrs(config, replicaId_);

        size_t nClients = config.clientIps.size();
        size_t nProxies = config.proxyIps.size();

        // First nClients addresses are for client connections
        for (size_t i = 0; i < nClients; i++) {
            clientAddrs_.push_back(addrPairs[i].second);
        }

        // Then proxy addresses
        for (size_t i = nClients; i < nClients + nProxies; i++) {
            proxyAddrs_.push_back(addrPairs[i].second);
        }

        // Remaining addresses are for replica-to-replica connections
        for (size_t i = nClients + nProxies; i < addrPairs.size(); i++) {
            replicaAddrs_.push_back(addrPairs[i].second);
        }

        replicaAddr_ = Address(config.replicaIps[replicaId_], config.replicaPort);
        endpoint_ = std::make_unique<NngEndpointThreaded>(addrPairs, true, replicaAddrs_[replicaId_]);

    } else if (config.transport == "simple-rpc") {
        std::vector<Address> addrs;
        replicaAddr_ = Address(config.replicaIps[replicaId], config.replicaPort);
        addrs.push_back(replicaAddr_);

        // Add proxy addresses for receiver functionality
        for (uint32_t i = 0; i < config.proxyIps.size(); i++) {
            addrs.push_back(Address(config.proxyIps[i], config.proxyForwardPort));
            proxyAddrs_.push_back(Address(config.proxyIps[i], config.proxyForwardPort));
        }

        size_t nClients = config.clientIps.size();
        for (int i = 0; i < config.clientIps.size(); i++) {
            std::string clientIp = config.clientIps[i];
            clientAddrs_.push_back(Address(clientIp, config.clientPort + i));
            addrs.push_back(Address(clientIp, config.clientPort + i));
            LOG(INFO) << "Client " << i << ": " << addrs.back();
        }

        // Add replica addresses
        for (uint32_t i = 0; i < config.replicaIps.size(); i++) {
            replicaAddrs_.push_back(Address(config.replicaIps[i], config.replicaPort));
            if (i != replicaId_) {
                addrs.push_back(Address(config.replicaIps[i], config.replicaPort));
            }
        }

        endpoint_ = std::make_unique<OOORPCEndpoint>(bindAddress, replicaPort, addrs, sendThreadpool_.size());
    } else {
        // UDP setup
        replicaAddr_ = Address(config.replicaIps[replicaId], config.replicaPort);

        for (uint32_t i = 0; i < config.replicaIps.size(); i++) {
            replicaAddrs_.push_back(Address(config.replicaIps[i], config.replicaPort));
        }

        for (uint32_t i = 0; i < config.proxyIps.size(); i++) {
            proxyAddrs_.push_back(Address(config.proxyIps[i], config.proxyForwardPort));
        }

        for (uint32_t i = 0; i < config.clientIps.size(); i++) {
            clientAddrs_.push_back(Address(config.clientIps[i], config.clientPort));
        }

        endpoint_ = std::make_unique<UDPEndpoint>(bindAddress, replicaPort, true);
    }

    // Set up timer for receiver deadline checking
    fwdTimer_ =
        std::make_unique<Timer>([](void *ctx, void *endpoint) { ((Replica *) ctx)->checkDeadlines(); }, 1000, this);
    ev_set_priority(fwdTimer_->evTimer_, EV_MAXPRI);
    endpoint_->RegisterTimer(fwdTimer_.get());

    // Register unified message handler
    endpoint_->RegisterMsgHandler([this](MessageHeader *msgHdr, byte *msgBuffer, Address *sender) {
        this->handleMessage(msgHdr, msgBuffer, sender);

        if (GetMicrosecondTimestamp() - lastCheckTime_ > 1000) {
            lastCheckTime_ = GetMicrosecondTimestamp();
            this->checkDeadlines();   // Check deadlines after each message
        }
    });

    endpoint_->RegisterSignalHandler([&]() {
        running_ = false;
        endpoint_->LoopBreak();
    });

    LOG(INFO) << "Starting verify threads for replica functionality";
    for (int i = 0; i < numVerifyThreads_; i++) {
        verifyThreads_.emplace_back(&Replica::verifyMessagesThd, this);
    }

    LOG(INFO) << "Starting verify threads for receiver functionality";
    uint32_t numReceiverVerifyThreads = config.replicaNumVerifyThreads;
    for (int i = 0; i < numReceiverVerifyThreads; i++) {
        receiverVerifyThreads_.emplace_back(&Replica::receiverVerifyThd, this, i);
    }

    processThread_ = std::thread(&Replica::processMessagesThd, this);

    LOG(INFO) << "Replica initialized successfully";
}

Replica::~Replica()
{
    running_ = false;

    for (std::thread &thd : verifyThreads_) {
        if (thd.joinable()) {
            thd.join();
        }
    }

    for (std::thread &thd : receiverVerifyThreads_) {
        if (thd.joinable()) {
            thd.join();
        }
    }

    if (processThread_.joinable()) {
        processThread_.join();
    }
}

void Replica::run()
{
    endpoint_->Connect();
    LOG(INFO) << "Starting unified replica main loop";
    endpoint_->LoopRun();

    LOG(INFO) << "Replica exited cleanly";
}

void Replica::handleMessage(MessageHeader *msgHdr, byte *msgBuffer, Address *sender)
{
    if (msgHdr->msgLen < 0) {
        return;
    }

    VLOG(6) << "Received message of type " << (int) msgHdr->msgType << " from " << *sender;

    // Handle receiver-specific messages (from proxies)
    if (msgHdr->msgType == MessageType::DOM_REQUEST) {
        receiveRequest(msgHdr, msgBuffer, sender);
        return;
    }

    if (msgHdr->msgType == MessageType::DOM_BATCH_REQUEST) {
        receiveBatchedRequests(msgHdr, msgBuffer, sender);
        return;
    }

    // Handle preserialization client requests (from clients to replica 0)
    if (msgHdr->msgType == MessageType::PS_CLIENT) {
        // Enqueue to verify queue - after verification, will be forwarded
        byte *msgStart = (byte *) msgHdr;
        verifyQueue_.enqueue(
            std::vector<byte>(msgStart, msgStart + sizeof(MessageHeader) + msgHdr->msgLen + msgHdr->sigLen)
        );
        return;
    }

    byte *msgStart = (byte *) msgHdr;

    // Handle replica-specific messages
    // Skip verification of our own messages and receiver messages
    if (sender->ip() == replicaAddrs_[replicaId_].ip()) {
        processQueue_.enqueue(
            std::vector<byte>(msgStart, msgStart + sizeof(MessageHeader) + msgHdr->msgLen + msgHdr->sigLen)
        );
        return;
    }

    // Queue for verification
    verifyQueue_.enqueue(
        std::vector<byte>(msgStart, msgStart + sizeof(MessageHeader) + msgHdr->msgLen + msgHdr->sigLen)
    );
}

// Receiver functionality implementation
void Replica::receiveRequest(MessageHeader *msgHdr, byte *msgBuffer, Address *sender)
{
    DOMRequest request;
    if (!request.ParseFromArray(msgBuffer, msgHdr->msgLen)) {
        LOG(ERROR) << "Unable to parse DOM_REQUEST message";
        return;
    }
    int64_t recv_time = GetMicrosecondTimestamp();
    VLOG(2) << "PERF event=receive c_id=" << request.client_id() << " c_seq=" << request.client_seq()
            << " delay=" << (int64_t) recv_time - request.send_time()
            << " deadline_offset=" << (int64_t) request.deadline() - (int64_t) request.send_time()
            << " send_time=" << request.send_time() << " replica_id=" << replicaId_;

    enqueueReceiverRequest(recv_time, request);

    // Send measurement replies back to the proxy

    if (recv_time - lastMeasurementTimes_[request.proxy_id()] > 5000) {
        lastMeasurementTimes_[request.proxy_id()] = recv_time;
        std::string senderIp = sender->ip();
        sendMeasurementReply(Address(senderIp, proxyPort_), recv_time - request.send_time(), request.send_time());
    }
}

void Replica::receiveBatchedRequests(MessageHeader *msgHdr, byte *msgBuffer, Address *sender)
{
    DOMBatchRequest batchRequest;
    if (!batchRequest.ParseFromArray(msgBuffer, msgHdr->msgLen)) {
        LOG(ERROR) << "Unable to parse DOM_BATCH_REQUEST message";
        return;
    }

    int64_t recv_time = GetMicrosecondTimestamp();
    VLOG(3) << "RECEIVE BATCH from proxy " << batchRequest.proxy_id() << " with " << batchRequest.requests_size()
            << " requests";

    for (int i = 0; i < batchRequest.requests_size(); i++) {
        DOMRequest &request = *batchRequest.mutable_requests(i);
        enqueueReceiverRequest(recv_time, request);
    }

    if (recv_time - lastMeasurementTimes_[batchRequest.proxy_id()] > 5000) {
        lastMeasurementTimes_[batchRequest.proxy_id()] = recv_time;
        std::string senderIp = sender->ip();
        sendMeasurementReply(
            Address(senderIp, proxyPort_), recv_time - batchRequest.send_time(), batchRequest.send_time()
        );
    }
}

void Replica::enqueueReceiverRequest(int64_t recv_time, DOMRequest &request)
{
    if (recv_time > request.deadline()) {
        request.set_late(true);
        VLOG(2) << "Request " << request.client_id() << ", " << request.client_seq() << " is late by "
                << recv_time - request.deadline() << "us";
    }

    uint64_t deadline = request.deadline();
    if (ignoreDeadlines_) {
        deadline = recv_time;
    }

    auto r = std::make_shared<ReceiverRequest>();
    r->request = request;
    r->deadline = request.deadline();
    r->clientId = request.client_id();
    r->verified = false;

    {
        std::lock_guard<std::mutex> guard(deadlineQueueMtx_);
        deadlineQueue_[{deadline, request.client_id()}] = r;
    }

    receiverVerifyQueue_.enqueue(r);

    // Send measurement replies back to the proxy

    if (recv_time - lastMeasurementTimes_[request.proxy_id()] > 5000) {
        lastMeasurementTimes_[request.proxy_id()] = recv_time;

        VLOG(6) << "Sending measurement reply to proxy " << request.proxy_id() << " "
                << proxyAddrs_[request.proxy_id()];

        sendThreadpool_.enqueueTask([=, this](byte *buffer) {
            MeasurementReply mReply;
            mReply.set_receiver_id(replicaId_);
            mReply.set_owd(recv_time - request.send_time());
            mReply.set_send_time(request.send_time());

            VLOG(6) << "Measurement reply: receiver_id=" << mReply.receiver_id() << " owd=" << mReply.owd()
                    << " send_time=" << mReply.send_time() << " proxy_addr=" << proxyAddrs_[request.proxy_id()];
            MessageHeader *replyHdr = endpoint_->PrepareProtoMsg(mReply, MessageType::MEASUREMENT_REPLY, buffer);
            endpoint_->SendPreparedMsgTo(proxyAddrs_[request.proxy_id()], replyHdr);
        });
    }
}

void Replica::sendMeasurementReply(const Address &dstAddr, uint64_t owd, uint64_t sendTime)
{
    sendThreadpool_.enqueueTask([=, this](byte *buffer) {
        MeasurementReply mReply;
        mReply.set_receiver_id(replicaId_);
        mReply.set_owd(owd);
        mReply.set_send_time(sendTime);

        VLOG(6) << "Measurement reply: receiver_id=" << mReply.receiver_id() << " owd=" << mReply.owd()
                << " send_time=" << mReply.send_time() << " proxy_addr=" << dstAddr;
        MessageHeader *replyHdr = endpoint_->PrepareProtoMsg(mReply, MessageType::MEASUREMENT_REPLY, buffer);
        endpoint_->SendPreparedMsgTo(dstAddr, replyHdr);
    });
}

void Replica::forwardRequest(const DOMRequest &request)
{
    uint64_t now = GetMicrosecondTimestamp();

    VLOG(5) << "Forwarding request " << now - request.deadline() << "us after deadline "
            << "c_id=" << request.client_id() << " c_seq=" << request.client_seq();

    numForwarded_++;
    lastFwdDeadline_ = request.deadline();

    // Serialize DOM request and enqueue to process queue
    std::string serializedRequest;
    if (!request.SerializeToString(&serializedRequest)) {
        LOG(ERROR) << "Failed to serialize DOM request";
        return;
    }

    MessageHeader header(DOM_REQUEST, serializedRequest.size(), 0);

    std::vector<byte> msg(sizeof(MessageHeader) + serializedRequest.size());
    memcpy(msg.data(), &header, sizeof(MessageHeader));
    memcpy(msg.data() + sizeof(MessageHeader), serializedRequest.data(), serializedRequest.size());

    processQueue_.enqueue(msg);
}

void Replica::checkDeadlines()
{
    std::lock_guard<std::mutex> guard(deadlineQueueMtx_);

    uint64_t now = GetMicrosecondTimestamp();
    auto it = deadlineQueue_.begin();
    while (it != deadlineQueue_.end() && it->first.first <= now) {
        VLOG(3) << "Deadline " << it->first.first << " reached now=" << now;

        if (!it->second->verified) {
            VLOG(3) << "Request not verified, waiting for next check";
            break;
        }

        forwardRequest(it->second->request);
        auto temp = std::next(it);
        deadlineQueue_.erase(it);
        it = temp;
    }

    int64_t nextCheck = deadlineQueue_.empty() ? 1000 : (int64_t) deadlineQueue_.begin()->first.first - now;
    nextCheck = std::max(1000l, nextCheck);

    endpoint_->ResetTimer(fwdTimer_.get(), nextCheck);
}

void Replica::receiverVerifyThd(int threadId)
{
    LOG(INFO) << "Starting receiver verify thread " << threadId;

    uint32_t numVerified = 0;
    std::shared_ptr<ReceiverRequest> request;
    while (running_) {
        if (!receiverVerifyQueue_.wait_dequeue_timed(request, 10000)) {
            continue;
        }

        ClientRequest clientHeader;
        MessageHeader *clientMsgHdr = (MessageHeader *) request->request.client_req().c_str();
        byte *clientBody = (byte *) (clientMsgHdr + 1);

        if (!clientHeader.ParseFromArray(clientBody, clientMsgHdr->msgLen)) {
            LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
            continue;
        }

        bool verified = false;
        if (useHMAC_) {
            verified = hmacProvider_.verify(clientMsgHdr, {NodeType::CLIENT, request->clientId});
        } else {
            verified = sigProvider_.verify(clientMsgHdr, {NodeType::CLIENT, request->clientId});
        }

        {
            std::lock_guard<std::mutex> guard(deadlineQueueMtx_);
            if (verified) {
                VLOG(4) << "Verified client signature for c_id=" << request->clientId
                        << " c_seq=" << request->request.client_seq();
                request->verified = true;
            } else {
                VLOG(1) << "Failed to verify client signature!";
                deadlineQueue_.erase({request->deadline, request->clientId});
            }
        }

        numVerified++;
    }
}

void Replica::verifyMessagesThd()
{
    // TODO we do some redundant work deserializing messages here
    std::vector<byte> msg;

    while (running_) {
        if (!verifyQueue_.wait_dequeue_timed(msg, 50000)) {
            continue;
        }

        MessageHeader *hdr = (MessageHeader *) msg.data();
        byte *body = (byte *) (hdr + 1);

        if (hdr->msgType == CERT) {
            Cert cert;

            if (!cert.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse CERT message";
                continue;
            }

            if (!verifyCert(cert)) {
                continue;
            }

            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == REPLY) {
            Reply reply;

            if (!reply.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPLY message";
                continue;
            }

            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, reply.replica_id()})) {
                LOG(INFO) << "Failed to verify replica signature for REPLY message for replica " << reply.replica_id();
                continue;
            }

            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == COMMIT) {
            Commit commitMsg;

            if (!commitMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse COMMIT message";
                continue;
            }

            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, commitMsg.replica_id()})) {
                LOG(INFO) << "Failed to verify replica signature!";
                continue;
            }

            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == SNAPSHOT_REQUEST) {
            SnapshotRequest request;
            if (!request.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse SNAPSHOT_REQUEST message";
                continue;
            }
            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, request.replica_id()})) {
                LOG(INFO) << "Failed to verify replica signature!";
                continue;
            }
            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == SNAPSHOT_REPLY) {
            SnapshotReply reply;
            if (!reply.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse SNAPSHOT_REPLY message";
                continue;
            }
            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, reply.replica_id()})) {
                LOG(INFO) << "Failed to verify replica signature!";
                continue;
            }
            if (!verifyCheckpoint(reply.checkpoint())) {
                LOG(INFO) << "Failed to verify checkpoint from replica " << reply.replica_id() << " in SNAPSHOT_REPLY!";
                continue;
            }

            if (reply.has_snapshot_checkpoint() && !verifyCheckpoint(reply.snapshot_checkpoint())) {
                LOG(INFO) << "Failed to verify snapshot checkpoint from replica " << reply.replica_id()
                          << " in SNAPSHOT_REPLY!";
                continue;
            }

            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == MISSING_REQUEST_FETCH) {
            dombft::proto::MissingRequestFetch fetchRequest;
            if (!fetchRequest.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse MISSING_REQUEST_FETCH message";
                continue;
            }
            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, fetchRequest.replica_id()})) {
                LOG(INFO) << "Failed to verify replica signature!";
                continue;
            }
            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == MISSING_REQUEST_REPLY) {
            dombft::proto::MissingRequestReply fetchReply;
            if (!fetchReply.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse MISSING_REQUEST_REPLY message";
                continue;
            }
            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, fetchReply.replica_id()})) {
                LOG(INFO) << "Failed to verify replica signature!";
                continue;
            }
            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == CLIENT_REQUEST) {
            ClientRequest requestMsg;

            if (!requestMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
                continue;
            }

            if (!sigProvider_.verify(hdr, {NodeType::CLIENT, requestMsg.client_id()})) {
                LOG(INFO) << "Failed to verify client signature!";
                continue;
            }

            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == PS_CLIENT) {
            ClientRequest requestMsg;

            if (!requestMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PS_CLIENT message";
                continue;
            }

            if (!sigProvider_.verify(hdr, {NodeType::CLIENT, requestMsg.client_id()})) {
                LOG(INFO) << "Failed to verify client signature on PS_CLIENT!";
                continue;
            }

            VLOG(3) << "PS_CLIENT verified c_id=" << requestMsg.client_id() << " c_seq=" << requestMsg.client_seq();

            // Just pass to processQueue - forwarding and sequencing happens in processMessagesThd
            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == PS_LEADER_FORWARD) {
            // Verify underlying client request
            // Don't bother verifying leader signature for this experiment for now
            PSLeaderForward fwd;
            if (!fwd.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PS_LEADER_FORWARD message";
                continue;
            }

            ClientRequest clientReq = fwd.request();
            std::string clientSignature = fwd.client_signature();

            if (!sigProvider_.verify(
                    clientReq.SerializeAsString(), clientSignature, {NodeType::CLIENT, clientReq.client_id()}
                )) {
                LOG(INFO) << "Failed to verify client signature on PS_LEADER_FORWARD!";
                continue;
            }

            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == PS_LEADER_ORDER) {
            PSLeaderOrder order;

            if (!order.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PS_LEADER_ORDER message";
                continue;
            }

            // No signature verification needed - if replica 0 equivocates progress wont be made anyways
            // and PS is not a full impementation
            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == REPAIR_TIMEOUT) {
            RepairTimeout timeoutMsg;

            if (!timeoutMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPAIR_TIMEOUT message";
                return;
            }

            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, timeoutMsg.replica_id()})) {
                LOG(INFO) << "Failed to verify replica signature!";
                continue;
            }

            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == REPAIR_REPLY_PROOF) {
            RepairReplyProof proofMsg;

            if (!proofMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPAIR_REPLY_PROOF message";
                return;
            }

            if (!verifyRepairReplyProof(proofMsg)) {
                // TODO should be LOG(WARNING)
                VLOG(2) << "Failed to verify repair reply proof!";
                continue;
            }

            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == REPAIR_TIMEOUT_PROOF) {
            RepairTimeoutProof proofMsg;

            if (!proofMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPAIR_TRIGGER message";
                return;
            }

            if (!verifyRepairTimeoutProof(proofMsg)) {
                LOG(WARNING) << "Failed to verify repair timeout proof!";
                continue;
            }

            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == REPAIR_START) {
            RepairStart repairStartMsg;
            if (!repairStartMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPAIR_START message";
                continue;
            }

            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, repairStartMsg.replica_id()})) {
                LOG(INFO) << "Failed to verify replica signature!";
                continue;
            }

            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == PBFT_PREPREPARE) {
            PBFTPrePrepare PBFTPrePrepareMsg;
            if (!PBFTPrePrepareMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PBFTPrePrepare message";
                continue;
            }
            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, PBFTPrePrepareMsg.primary_id()})) {
                LOG(INFO) << "Failed to verify primary replica signature!";
                continue;
            }

            // Verify digest and logs in proposal
            RepairProposal proposal = PBFTPrePrepareMsg.proposal();
            if (getProposalDigest(proposal) != PBFTPrePrepareMsg.proposal_digest()) {
                LOG(INFO) << "Proposal digest does not match!";
                continue;
            }

            if (!verifyRepairProposal(proposal)) {
                LOG(INFO) << "Failed to verify repair proposal!";
                assert(false);   // TODO remove later
                continue;
            }

            processQueue_.enqueue(msg);
        } else if (hdr->msgType == PBFT_PREPARE) {
            PBFTPrepare PBFTPrepareMsg;
            if (!PBFTPrepareMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PBFTPrepare message";
                continue;
            }
            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, PBFTPrepareMsg.replica_id()})) {
                LOG(INFO) << "Failed to verify primary replica signature!";
                continue;
            }
            processQueue_.enqueue(msg);
        } else if (hdr->msgType == PBFT_COMMIT) {
            PBFTCommit PBFTCommitMsg;
            if (!PBFTCommitMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PBFTCommit message";
                continue;
            }
            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, PBFTCommitMsg.replica_id()})) {
                LOG(INFO) << "Failed to verify primary replica signature!";
                continue;
            }
            processQueue_.enqueue(msg);
        } else if (hdr->msgType == VIEW_UPDATE) {
            ViewUpdate viewUpdateMsg;

            if (!viewUpdateMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PBFTViewChange message";
                continue;
            }
            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, viewUpdateMsg.replica_id()})) {
                LOG(INFO) << "Failed to verify primary replica signature!";
                continue;
            }

            processQueue_.enqueue(msg);
        } else {
            // DOM_Requests from the receiver skip this step. We should drop
            // request types from other processes.
            LOG(ERROR) << "Verify thread does not handle message with unknown type " << (int) hdr->msgType;
        }
    }
}

void Replica::processMessagesThd()
{
    // TODO we do some redundant work deserializing messages here
    std::vector<byte> msg;

    while (running_) {
        // Check for timeouts each time before processing a message
        checkTimeouts();

        // Check to see if any snapshots are ready
        std::pair<uint32_t, AppSnapshot> snapshot;
        if (snapshotQueue_.try_dequeue(snapshot)) {
            processSnapshot(snapshot.second, snapshot.first);
        }

        if (!processQueue_.wait_dequeue_timed(msg, 100000)) {
            continue;
        }

        MessageHeader *hdr = (MessageHeader *) msg.data();
        byte *body = (byte *) (hdr + 1);

        if (hdr->msgType == MessageType::DOM_REQUEST) {
            DOMRequest domHeader;
            ClientRequest clientHeader;

            if (!domHeader.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse DOM_REQUEST message";
                return;
            }

            // Separate this out into another function probably.
            MessageHeader *clientMsgHdr = (MessageHeader *) domHeader.client_req().c_str();
            byte *clientBody = (byte *) (clientMsgHdr + 1);
            if (!clientHeader.ParseFromArray(clientBody, clientMsgHdr->msgLen)) {
                LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
                continue;
            }

            // TODO Hack to pass through deadline lol
            clientHeader.set_deadline(domHeader.deadline());

            if (repair_) {
                VLOG(6) << "Queuing request due to repair";
                repairQueuedReqs_.insert({{domHeader.deadline(), clientHeader.client_id()}, clientHeader});

                // Check if this request matches any pending missing requests
                if (missingRequestFetchSent_) {
                    RequestId reqKey = {clientHeader.client_id(), clientHeader.client_seq()};
                    auto it = std::find(pendingMissingRequests_.begin(), pendingMissingRequests_.end(), reqKey);
                    if (it != pendingMissingRequests_.end()) {
                        LOG(INFO) << "Received missing request from client c_id=" << clientHeader.client_id()
                                  << " c_seq=" << clientHeader.client_seq();
                        pendingMissingRequests_.erase(it);

                        // If all missing requests are now available, retry repair
                        if (pendingMissingRequests_.empty()) {
                            LOG(INFO) << "All missing requests now available (via client), retrying repair";
                            missingRequestFetchSent_ = false;

                            if (repair_)
                                tryFinishRepair();
                        }
                    }
                }

                continue;
            }

            if (swapFreq_ && log_->getNextSeq() % swapFreq_ == 0)
                holdAndSwapCliReq(clientHeader);
            else
                processClientRequest(clientHeader);
        }

        else if (hdr->msgType == CLIENT_REQUEST) {
            ClientRequest clientHeader;

            if (!clientHeader.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
                continue;
            }

            processClientRequest(clientHeader);
        }

        else if (hdr->msgType == CERT) {
            Cert cert;

            if (!cert.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse CERT message";
                return;
            }

            processCert(cert);
        } else if (hdr->msgType == REPLY) {
            Reply replyHeader;

            if (!replyHeader.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPLY message";
                return;
            }

            processReply(replyHeader, std::span{body + hdr->msgLen, hdr->sigLen});
        } else if (hdr->msgType == COMMIT) {
            Commit commitMsg;

            if (!commitMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse COMMIT message";
                return;
            }

            processCommit(commitMsg, std::span{body + hdr->msgLen, hdr->sigLen});
        }

        else if (hdr->msgType == SNAPSHOT_REQUEST) {
            SnapshotRequest request;
            if (!request.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse SNAPSHOT_REQUEST message";
                return;
            }
            processSnapshotRequest(request);
        } else if (hdr->msgType == SNAPSHOT_REPLY) {
            SnapshotReply reply;
            if (!reply.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse SNAPSHOT_REPLY message";
                return;
            }
            processSnapshotReply(reply);
        }

        else if (hdr->msgType == MISSING_REQUEST_FETCH) {
            dombft::proto::MissingRequestFetch fetchRequest;
            if (!fetchRequest.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse MISSING_REQUEST_FETCH message";
                return;
            }
            processMissingRequestFetch(fetchRequest);
        }

        else if (hdr->msgType == MISSING_REQUEST_REPLY) {
            dombft::proto::MissingRequestReply fetchReply;
            if (!fetchReply.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse MISSING_REQUEST_REPLY message";
                return;
            }
            processMissingRequestReply(fetchReply);
        }

        else if (hdr->msgType == REPAIR_TIMEOUT) {
            RepairTimeout msg;

            if (!msg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPAIR_TIMEOUT message";
                return;
            }

            processRepairTimeout(msg, std::span{body + hdr->msgLen, hdr->sigLen});
        }

        else if (hdr->msgType == REPAIR_REPLY_PROOF) {
            RepairReplyProof msg;

            if (!msg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPAIR_REPLY_PROOF message";
                return;
            }

            processRepairReplyProof(msg);
        }

        else if (hdr->msgType == REPAIR_TIMEOUT_PROOF) {
            RepairTimeoutProof msg;

            if (!msg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPAIR_TRIGGER message";
                return;
            }

            processRepairTimeoutProof(msg);
        }

        else if (hdr->msgType == REPAIR_START) {
            RepairStart msg;

            if (!msg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPAIR_START message";
                return;
            }
            processRepairStart(msg, std::span{body + hdr->msgLen, hdr->sigLen});
        }

        else if (hdr->msgType == PBFT_PREPREPARE) {
            PBFTPrePrepare msg;

            if (!msg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PBFT_PREPREPARE message";
                return;
            }

            processPrePrepare(msg);
        }

        else if (hdr->msgType == PBFT_PREPARE) {
            PBFTPrepare msg;

            if (!msg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PBFT_PREPARE message";
                return;
            }

            processPrepare(msg, std::span{body + hdr->msgLen, hdr->sigLen});
        }

        else if (hdr->msgType == PBFT_COMMIT) {
            PBFTCommit msg;

            if (!msg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PBFT_COMMIT message";
                return;
            }

            processPBFTCommit(msg, std::span{body + hdr->msgLen, hdr->sigLen});
        }

        else if (hdr->msgType == VIEW_UPDATE) {
            ViewUpdate viewUpdateMsg;

            if (!viewUpdateMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PBFTViewChange message";
                continue;
            }

            updateReplicaView(viewUpdateMsg.replica_id(), viewUpdateMsg.view(), viewUpdateMsg.round());
        }

        else if (hdr->msgType == PS_CLIENT) {
            ClientRequest clientHeader;

            if (!clientHeader.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PS_CLIENT message in processMessagesThd";
                continue;
            }

            processPSClient(clientHeader, std::span{body + hdr->msgLen, hdr->sigLen});

        }

        else if (hdr->msgType == PS_LEADER_FORWARD) {
            PSLeaderForward fwd;

            if (!fwd.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PS_LEADER_FORWARD message";
                continue;
            }

            processPSLeaderForward(fwd);
        }

        else if (hdr->msgType == PS_LEADER_ORDER) {
            PSLeaderOrder order;

            if (!order.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PS_LEADER_ORDER message";
                continue;
            }

            processPSLeaderOrder(order);
        }

        else {
            LOG(ERROR) << "Process thread does not handle message with unknown type " << (int) hdr->msgType;
        }
    }
}

void Replica::processClientRequest(const ClientRequest &request, bool queued)
{
    uint32_t clientId = request.client_id();
    uint32_t clientSeq = request.client_seq();

    if (clientId < 0 || clientId > clientAddrs_.size()) {
        LOG(ERROR) << "Invalid client id" << clientId;
        return;
    }

    // 1. Check if client request has been executed in latest checkpoint (i.e. is committed), in which case
    // we should return a CommittedReply, and client only needs f + 1
    if (log_->getCommittedCheckpoint().clientRecord_.contains(clientId, clientSeq)) {
        VLOG(4) << "DUP request c_id=" << clientId << " c_seq=" << clientSeq
                << " has been committed in previous checkpoint/repair!, Sending committed reply";

        CommittedReply reply;

        reply.set_replica_id(replicaId_);
        reply.set_client_id(clientId);
        reply.set_client_seq(clientSeq);
        reply.set_is_repair(false);

        // TODO Cache for client results not implemented, so blank results are sent that all look the same

        sendMsgToDst(reply, MessageType::COMMITTED_REPLY, clientAddrs_[clientId]);
        return;
    }

    std::string result;
    uint32_t seq;

    if (!log_->addEntry(clientId, clientSeq, request.req_data(), result)) {
        VLOG(2) << "DUP request c_id=" << clientId << " c_seq=" << clientSeq
                << " is added into log, but not committed, dropping!";
        return;
    }

    seq = log_->getNextSeq() - 1;
    log_->getEntry(seq).deadline = request.deadline();

    VLOG(2) << "PERF event=spec_execute replica_id=" << replicaId_ << " seq=" << seq << " client_id=" << clientId
            << " client_seq=" << clientSeq << " round=" << round_ << " digest=" << digest_to_hex(log_->getDigest())
            << " queued=" << queued << " request_size=" << request.req_data().size();

    Reply reply;
    reply.set_client_id(clientId);
    reply.set_client_seq(clientSeq);
    reply.set_replica_id(replicaId_);
    reply.set_result(result);
    reply.set_seq(seq);
    reply.set_round(round_);
    reply.set_digest(log_->getDigest());
    reply.set_queued(queued);

    sendMsgToDst(reply, MessageType::REPLY, clientAddrs_[clientId]);

    // Try and commit every checkpointInterval replies
    if (seq % checkpointInterval_ == 0) {
        // Save a digest of the application state and also save a snapshot
        startCheckpoint(seq % snapshotInterval_ == 0);

        checkpointTimeoutStart_ = GetMicrosecondTimestamp();
    }

    if (seq == 1) {
        checkpointTimeoutStart_ = GetMicrosecondTimestamp();
    }
}

void Replica::holdAndSwapCliReq(const proto::ClientRequest &request)
{

    uint32_t clientId = request.client_id();
    uint32_t clientSeq = request.client_seq();
    if (!heldRequest_) {
        heldRequest_ = request;
        VLOG(2) << "Holding request (" << clientId << ", " << clientSeq << ") for swapping";
        return;
    }
    processClientRequest(request);
    processClientRequest(heldRequest_.value());
    VLOG(2) << "Swapped requests (" << clientId << ", " << clientSeq << ") and (" << heldRequest_->client_id() << ", "
            << heldRequest_->client_seq() << ")";
    heldRequest_.reset();
}

void Replica::processCert(const Cert &cert)
{
    // TODO make sure this works
    const Reply &r = cert.replies()[0];
    CertReply reply;

    if (cert.round() < round_) {
        VLOG(2) << "Received stale cert with round " << cert.round() << " < " << round_ << " for seq=" << r.seq()
                << " c_id=" << r.client_id() << " c_seq=" << r.client_seq();
        return;
    }

    if (repair_) {
        VLOG(2) << "Cannot accept cert for seq=" << r.seq() << " from " << r.replica_id() << " due to ongoing repair";
        return;
    }

    if (!log_->addCert(cert.seq(), cert)) {
        VLOG(2) << "Failed to add cert for seq=" << r.seq() << " c_id=" << r.client_id() << " c_seq=" << r.client_seq();
        return;
    }

    reply.set_replica_id(replicaId_);
    reply.set_round(round_);
    reply.set_client_id(r.client_id());
    reply.set_client_seq(r.client_seq());
    reply.set_seq(r.seq());

    VLOG(3) << "Sending cert ack for seq=" << r.seq() << " c_id=" << reply.client_id()
            << " cseq=" << reply.client_seq();

    sendMsgToDst(reply, MessageType::CERT_REPLY, clientAddrs_[reply.client_id()]);
}

void Replica::processReply(const dombft::proto::Reply &reply, std::span<byte> sig)
{
    uint32_t rSeq = reply.seq();

    if (repair_) {
        VLOG(2) << "Ignoring reply for seq=" << rSeq << " from " << reply.replica_id() << " due to ongoing repair";
        return;
    }

    if (reply.round() < round_) {
        VLOG(4) << "Checkpoint reply seq=" << rSeq << " round outdated, skipping";
        return;
    }
    VLOG(3) << "Processing reply from replica " << reply.replica_id() << " for seq " << rSeq;

    // If we receive a reply for a checkpoint for a sequence number that is not a multiple of the
    // checkpoint interval, some replica has timed out waiting to reach the checkpoint interval.
    if (reply.seq() % checkpointInterval_ != 0) {
        bool alreadyStarted = checkpointCollectors_.hasCollector(round_, reply.seq());
        bool alreadyCommitted = reply.seq() <= log_->getCommittedCheckpoint().seq;

        auto [round, seq] = checkpointTimeoutSeqs_[reply.replica_id()];
        bool alreadyTried =
            seq != 0 && (round == round_) && ((reply.seq() / checkpointInterval_) == (seq / checkpointInterval_));

        if (!alreadyStarted && !alreadyCommitted && !alreadyTried) {
            VLOG(1) << "PERF event=checkpoint_timeout_reply" << " seq=" << reply.seq() << " round=" << round_
                    << " replica_id=" << reply.replica_id() << " self_id=" << replicaId_ << " checkpoint_seq=" << seq;

            checkpointTimeoutSeqs_[reply.replica_id()] = {round_, reply.seq()};
            startCheckpoint(false);
        }
    }

    auto &checkpoint = log_->getCommittedCheckpoint();

    if (rSeq <= checkpoint.seq) {
        VLOG(4) << "Checkpoint reply with seq=" << rSeq << " is already committed, ignoring!"
                << ". Current checkpoint seq is " << checkpoint.seq;
        return;
    }

    if (!checkpointCollectors_.hasCollector(round_, rSeq)) {
        VLOG(4) << "Checkpoint collector does not exist for seq=" << rSeq << " round=" << round_
                << " creating one now ";

        if (!checkpointCollectors_.initCollector(round_, rSeq, rSeq % snapshotInterval_ == 0)) {
            return;
        }
    }
    CheckpointCollector &coll = checkpointCollectors_.at(round_, rSeq);

    if (coll.commitReady()) {
        VLOG(4) << "COMMIT already sent for seq=" << rSeq << " round=" << round_ << " ignoring";
        return;
    }

    if (coll.addAndCheckReply(reply, sig)) {
        assert(reply.round() == round_);

        bool normalPathEnabled = ConfigManager::getInstance().getConfig().clientNormalPathEnabled;

        if (normalPathEnabled) {
            dombft::proto::Cert cert;
            coll.getCert(cert);

            if (!log_->addCert(rSeq, cert)) {
                VLOG(2) << "CHECKPOINT: Failed to add cert for seq=" << rSeq;
                return;
            }
        }

        if (coll.commitReady()) {
            Commit commit;
            coll.getOwnCommit(commit);

            VLOG(3) << "Sending own COMMIT seq=" << commit.seq() << " round=" << commit.round()
                    << " digest=" << digest_to_hex(commit.log_digest())
                    << " app_digest=" << digest_to_hex(commit.app_digest());

            broadcastToReplicas(commit, MessageType::COMMIT);
        }

        return;
    } else if (coll.hasConflictProof()) {
        VLOG(1) << "PERF event=checkpoint_conflict"
                << " seq=" << rSeq << " round=" << round_ << " replica_id=" << replicaId_;

        dombft::proto::RepairReplyProof replyProof;
        coll.getConflictProof(replyProof);

        std::ostringstream oss;
        oss << "round=" << round_ << "\n";
        for (int i = 0; i < replyProof.replies().size(); i++) {
            const auto &reply = replyProof.replies(i);
            oss << reply.replica_id() << " " << digest_to_hex(reply.digest()) << " " << reply.seq() << " "
                << reply.round() << "\n";
        }
        VLOG(1) << "Conflict proof:\n" << oss.str();

        replyProof.set_replica_id(replicaId_);
        replyProof.set_round(round_);
        replyProof.set_view(pbftView_);

        broadcastToReplicas(replyProof, MessageType::REPAIR_REPLY_PROOF);

        startRepair();
    }
}

void Replica::processSnapshot(const AppSnapshot &snapshot, uint32_t round)
{
    VLOG(4) << "CHECKPOINT Processing snapshot for seq=" << snapshot.seq << " round=" << round;

    if (!checkpointCollectors_.hasCollector(round, snapshot.seq)) {
        VLOG(4) << "CHECKPOINT Collector does not exist for round=" << round << " seq = " << snapshot.seq
                << " which means this was either committed or skipped";
        return;
    }

    CheckpointCollector &coll = checkpointCollectors_.at(round, snapshot.seq);
    coll.addOwnSnapshot(snapshot);

    if (coll.commitReady()) {
        Commit commit;
        coll.getOwnCommit(commit);

        VLOG(3) << "CHECKPOINT Sending own COMMIT seq=" << commit.seq() << " round=" << commit.round()
                << " digest=" << digest_to_hex(commit.log_digest())
                << " app_digest=" << digest_to_hex(commit.app_digest());

        broadcastToReplicas(commit, MessageType::COMMIT);
    }
}

void Replica::processCommit(const dombft::proto::Commit &commit, std::span<byte> sig)
{
    uint32_t seq = commit.seq();
    VLOG(3) << "Processing COMMIT from replica " << commit.replica_id() << " for seq " << seq
            << " round=" << commit.round() << " digest=" << digest_to_hex(commit.log_digest())
            << " app_digest=" << digest_to_hex(commit.app_digest());

    if (!checkpointCollectors_.hasCollector(commit.round(), seq)) {
        VLOG(4) << "Checkpoint collector does not exist for seq=" << seq << " round=" << commit.round()
                << " creating one now";
        if (!checkpointCollectors_.initCollector(
                commit.round(), seq, seq % snapshotInterval_ == 0 || commit.has_app_digest()
            )) {
            return;
        }
    }
    CheckpointCollector &coll = checkpointCollectors_.at(commit.round(), seq);

    // add current commit msg to collector
    // use the majority agreed commit message if exists
    if (coll.addAndCheckCommit(commit, sig)) {
        // TODO we can update our round in case commit.round() > round_
        // if (round_ < commit.round()) {
        //     LOG(WARNING) << "Ignoring checkpoint for future round " << commit.round() << " current round is " <<
        //     round_; return;
        // }

        ::LogCheckpoint checkpoint;
        coll.getCheckpoint(checkpoint);
        uint32_t seq = checkpoint.seq;

        if (checkpointDropFreq_ && seq / checkpointInterval_ % checkpointDropFreq_ == 0) {
            LOG(INFO) << "Dropping checkpoint seq=" << seq;
            return;
        }

        LOG(INFO) << "Trying to commit  seq=" << seq << " commit_digest=" << digest_to_hex(checkpoint.logDigest)
                  << " in round=" << commit.round();

        if (seq >= log_->getNextSeq() || log_->getDigest(seq) != checkpoint.logDigest) {
            // TODO choose a random replica from those that have this
            assert(!checkpoint.commits.empty());
            uint32_t replicaId =
                std::next(checkpoint.commits.begin(), GetMicrosecondTimestamp() % checkpoint.commits.size())
                    ->second.replica_id();
            if (seq >= log_->getNextSeq()) {
                LOG(INFO) << "My log is behind round=" << round_ << " nextSeq=" << log_->getNextSeq();
            } else {
                LOG(INFO) << "My log digest " << digest_to_hex(log_->getDigest(seq))
                          << " does not match the commit message digest " << digest_to_hex(checkpoint.logDigest);
            }

            // This can cause replica to fall behind; by the time it gets a snapshot, it would already be too far
            // behind
            if (!dombft::ConfigManager::getInstance().getConfig().replicaSkipAlignment) {
                if (!checkpointSnapshotRequested_ || seq >= log_->getNextSeq() + 5 * checkpointInterval_) {
                    VLOG(1) << "PERF event=align_start seq=" << seq
                            << " log_digest=" << digest_to_hex(checkpoint.logDigest)
                            << " app_digest=" << digest_to_hex(checkpoint.appDigest) << " replica_id=" << replicaId_;

                    sendSnapshotRequest(replicaId, checkpoint.seq);
                }
                checkpointSnapshotRequested_ = true;
            } else {
                LOG(INFO) << "Skipping alignment snapshot request due to skipAlignment config";
            }

        } else if (!coll.needsSnapshot()) {
            log_->setCheckpoint(checkpoint);

            VLOG(1) << "PERF event=checkpoint_end snapshot=false seq=" << seq
                    << " log_digest=" << digest_to_hex(checkpoint.logDigest)
                    << " app_digest=" << digest_to_hex(checkpoint.appDigest);

            // if there is overlapping and later checkpoint commits first, skip earlier ones
            checkpointCollectors_.cleanStaleCollectors(
                log_->getStableCheckpoint().seq, log_->getCommittedCheckpoint().seq
            );

        } else if (checkpoint.snapshot != nullptr) {
            log_->setCheckpoint(checkpoint);

            VLOG(1) << "PERF event=checkpoint_end snapshot=true seq=" << seq
                    << " log_digest=" << digest_to_hex(checkpoint.logDigest)
                    << " app_digest=" << digest_to_hex(checkpoint.appDigest);

            // if there is overlapping and later checkpoint commits first, skip earlier ones
            checkpointCollectors_.cleanStaleCollectors(
                log_->getStableCheckpoint().seq, log_->getCommittedCheckpoint().seq
            );
        } else {
            VLOG(4) << "CHECKPOINT: Quorum of commits match our log, but we do not have a snapshot yet!"
                    << " Will wait for our snapshot request to finish and then receive our own commit!";
            // TODO, if a replica needed a snapshot to catch up, this case may happen and then the
            // snapshot will never be taken. This would get cleaned up next time...
        }

        // Unregister repair timer set by client as checkpoint confirms progress
        repairTimeoutStart_ = 0;
    }
}

void Replica::startCheckpoint(bool createSnapshot)
{
    uint32_t seq = log_->getNextSeq() - 1;

    if (createSnapshot) {
        uint32_t round = round_;
        app_->takeSnapshot([&, round](const AppSnapshot &snapshot) {
            // Queue to be processed by main thread
            snapshotQueue_.enqueue({round, snapshot});
        });
    }

    if (checkpointCollectors_.hasCollector(round_, seq)) {
        VLOG(4) << "Checkpoint collector already exists for seq=" << seq << " round=" << round_
                << " since we received messages from other replicas";
    } else {

        if (!checkpointCollectors_.initCollector(round_, seq, createSnapshot)) {
            return;
        }
    }

    checkpointCollectors_.at(round_, seq).addOwnState(log_->getDigest(seq), log_->getClientRecord());

    Reply reply;
    const ::LogEntry &entry = log_->getEntry(seq);   // TODO better namespace

    reply.set_client_id(entry.client_id);
    reply.set_client_seq(entry.client_seq);
    reply.set_replica_id(replicaId_);
    reply.set_round(round_);
    reply.set_seq(entry.seq);
    reply.set_digest(entry.digest);

    VLOG(1) << "PERF event=checkpoint_start seq=" << seq << " createSnapshot=" << createSnapshot << " round=" << round_
            << " log_digest=" << digest_to_hex(log_->getDigest());

    broadcastToReplicas(reply, MessageType::REPLY);
}

void Replica::processSnapshotRequest(const SnapshotRequest &request)
{
    uint32_t reqSeq = request.seq();
    uint32_t round = request.round();
    VLOG(1) << "Processing SNAPSHOT_REQUEST from replica " << request.replica_id() << " for seq " << reqSeq
            << " from sequnece " << request.last_checkpoint_seq();

    if (reqSeq > log_->getCommittedCheckpoint().seq) {
        VLOG(4) << "Requested req " << reqSeq << " is ahead of current checkpoint, cannot provide snapshot";
        return;
    }
    // In fact, returned state snapshot is always the latest snapshot (with the checkpoint.seq)
    // the LOG is just to make it clear.

    // TODO, use request's last checkpoint sequence to return a delta instead..

    SnapshotReply snapshotReply;
    snapshotReply.set_round(round_);
    snapshotReply.set_seq(log_->getCommittedCheckpoint().seq);
    snapshotReply.set_replica_id(replicaId_);

    const ::LogCheckpoint &committedCp = log_->getCommittedCheckpoint();
    committedCp.toProto(*snapshotReply.mutable_checkpoint());

    const ::LogCheckpoint &stableCp = log_->getStableCheckpoint();
    if (request.last_checkpoint_seq() < stableCp.seq) {
        VLOG(1) << "Snapshot request too far back, sending application snapshot!";

        assert(stableCp.snapshot != nullptr);

        snapshotReply.set_snapshot(*stableCp.snapshot);
        stableCp.toProto(*snapshotReply.mutable_snapshot_checkpoint());
    }

    // Adding requests not included in snapshot up to my committed_seq
    uint32_t startSeq = std::max(request.last_checkpoint_seq(), stableCp.seq) + 1;
    for (uint32_t seq = startSeq; seq <= log_->getCommittedCheckpoint().seq; seq++) {
        auto &entry = log_->getEntry(seq);

        dombft::proto::LogEntry entryProto;
        entry.toProto(entryProto);
        entryProto.set_request(entry.request);
        (*snapshotReply.add_log_entries()) = entryProto;
    }

    VLOG(1) << "Sending SNAPSHOT_REPLY to " << request.replica_id() << " round=" << snapshotReply.round() << " from "
            << startSeq << " to " << log_->getCommittedCheckpoint().seq
            << " (app snapshot=" << snapshotReply.has_snapshot() << ")";

    sendMsgToDst(snapshotReply, MessageType::SNAPSHOT_REPLY, replicaAddrs_[request.replica_id()]);
}

void Replica::processSnapshotReply(const dombft::proto::SnapshotReply &snapshotReply)
{
    if (snapshotReply.round() < round_) {
        VLOG(1) << "Snapshot reply round outdated, skipping";

        // TODO these are probably not necessary
        repairSnapshotRequested_ = false;
        checkpointSnapshotRequested_ = false;
        return;
    }
    if (snapshotReply.seq() <= log_->getCommittedCheckpoint().seq) {
        VLOG(1) << "Seq " << snapshotReply.seq() << " is already committed, skipping snapshot reply";
        repairSnapshotRequested_ = false;
        checkpointSnapshotRequested_ = false;

        return;
    }
    if (repair_) {

        // Finish applying LogSuffix computed from repair proposal and return to normal processing

        // This prevents us from accidentally ressettiting to some old state
        if (!repairSnapshotRequested_ && snapshotReply.round() <= round_) {
            LOG(ERROR) << "Received snapshot reply during repair before repair finishes due to previous checkpoint, "
                          "ignoring... !";
            return;
        }

        if (!repairProposal_.has_value()) {
            LOG(ERROR) << "Received snapshot reply during repair but no repair proposal exists, ignoring... !";
            return;
        }

        LogSuffix &logSuffix = getRepairLogSuffix();
        if (snapshotReply.seq() < logSuffix.checkpoint->seq()) {
            LOG(ERROR) << "Received snapshot reply during repair that is too old, ignoring... !"
                       << " Previous checkpoint for seq=" << snapshotReply.seq()
                       << " need seq=" << logSuffix.checkpoint->seq();
            return;
        }

        uint32_t startSeq = snapshotReply.log_entries().size() > 0 ? snapshotReply.log_entries(0).seq()
                                                                   : snapshotReply.snapshot_checkpoint().seq();

        LOG(INFO) << "Processing (during repair) SNAPSHOT_REPLY from replica " << snapshotReply.replica_id()
                  << " for seq " << snapshotReply.seq() << " with log from " << startSeq
                  << " has_snapshot=" << snapshotReply.has_snapshot();

        std::vector<::ClientRequest> abortedRequests = getAbortedEntries(logSuffix, log_, curRoundStartSeq_);

        // TODO this may be redudnant
        std::map<RequestId, std::string> availableReqs;
        for (uint32_t seq = log_->getCommittedCheckpoint().seq + 1; seq < log_->getNextSeq(); seq++) {
            auto &entry = log_->getEntry(seq);
            availableReqs[{entry.client_id, entry.client_seq}] = entry.request;
        }

        for (auto [_, req] : repairQueuedReqs_) {
            availableReqs[{req.client_id(), req.client_seq()}] = req.req_data();
        }

        if (!log_->resetToSnapshot(snapshotReply)) {
            // TODO handle this case properly by retrying on another replica
            LOG(ERROR) << "Failed to reset log to snapshot, snapshot did not match digest!";
            throw std::runtime_error("Snapshot digest mismatch");
        }

        // if there is overlapping and later checkpoint commits first, skip earlier ones
        checkpointCollectors_.cleanStaleCollectors(log_->getStableCheckpoint().seq, log_->getCommittedCheckpoint().seq);

        if (snapshotReply.checkpoint().seq() > logSuffix.checkpoint->seq()) {
            LOG(WARNING) << "Snapshot is from future: seq=" << snapshotReply.seq() << " round=" << snapshotReply.round()
                         << ". We requested seq=" << logSuffix.checkpoint->seq() << " round=" << round_
                         << " repair_last_seq=" << logSuffix.checkpoint->seq() + logSuffix.entries.size()
                         << ". Still applying it before finishRepair, but will likely drop lots of messages";
        }

        std::vector<std::pair<uint32_t, uint32_t>> missingRequests;
        if (!applySuffix(logSuffix, availableReqs, log_, missingRequests)) {
            LOG(INFO) << "applySuffix failed due to missing requests, requesting from other replicas";
            sendMissingRequestFetch(missingRequests);
            return;
        }
        finishRepair(abortedRequests);

        // TODO temporary fix for issue #120, this may lead to later issues though
        round_ = std::max(round_, snapshotReply.round());
    } else if (!dombft::ConfigManager::getInstance().getConfig().replicaSkipAlignment) {
        // Apply snapshot from checkpoint and reorder my log
        // TODO make sure this isn't outdated...

        uint32_t startSeq = snapshotReply.log_entries().size() > 0 ? snapshotReply.log_entries(0).seq()
                                                                   : snapshotReply.snapshot_checkpoint().seq();

        LOG(INFO) << "Processing SNAPSHOT_REPLY from replica " << snapshotReply.replica_id() << " for seq "
                  << snapshotReply.seq() << " with log from " << startSeq
                  << " has_snapshot=" << snapshotReply.has_snapshot();

        if (!log_->applySnapshotModifyLog(snapshotReply)) {
            LOG(ERROR) << "Failed to apply snapshot because it did not match digest!";
            // TODO handle this better by requesting from another replica..
            throw std::runtime_error("Snapshot digest mismatch");
        }

        // TODO temporary fix for issue #120, this may lead to later issues though
        round_ = std::max(round_, snapshotReply.round());

        // if there is overlapping and later checkpoint commits first, skip earlier ones
        checkpointCollectors_.cleanStaleCollectors(log_->getStableCheckpoint().seq, log_->getCommittedCheckpoint().seq);

        // Resend replies after modifying log
        for (int seq = log_->getCommittedCheckpoint().seq + 1; seq < log_->getNextSeq(); seq++) {
            auto &entry = log_->getEntry(seq);

            Reply reply;

            reply.set_client_id(entry.client_id);
            reply.set_client_seq(entry.client_seq);
            reply.set_replica_id(replicaId_);
            reply.set_result(entry.result);
            reply.set_seq(seq);
            reply.set_round(round_);
            reply.set_digest(entry.digest);

            VLOG(2) << "PERF event=update_digest seq=" << seq << " digest=" << digest_to_hex(entry.digest)
                    << " c_id=" << entry.client_id << " c_seq=" << entry.client_seq;

            sendMsgToDst(reply, MessageType::REPLY, clientAddrs_[entry.client_id]);
        }

        VLOG(1) << "PERF event=align replicaId=" << replicaId_
                << " checkpoint_seq=" << log_->getCommittedCheckpoint().seq << " log_seq=" << log_->getNextSeq() - 1
                << " log_digest=" << digest_to_hex(log_->getDigest());
    } else {
        LOG(WARNING) << "Ignoring snapshot reply due to replicaSkipAlignment=true!";
    }

    // Got the snapshot
    repairSnapshotRequested_ = false;
    checkpointSnapshotRequested_ = false;
}

void Replica::processMissingRequestFetch(const dombft::proto::MissingRequestFetch &fetchRequest)
{
    LOG(INFO) << "Processing MISSING_REQUEST_FETCH from replica " << fetchRequest.replica_id() << " for "
              << fetchRequest.request_ids_size() << " requests";

    // if (fetchRequest.round() < round_) {
    //     VLOG(1) << "Missing request fetch round outdated, skipping";
    //     return;
    // }

    dombft::proto::MissingRequestReply reply;
    reply.set_round(fetchRequest.round());
    reply.set_replica_id(replicaId_);

    // TODO this is inefficient, we should iterate once trhough the logs
    // TODO we run into issues if the request was truncated from the log....
    // Iterate through requested requests and search for them
    for (const auto &reqId : fetchRequest.request_ids()) {
        uint32_t clientId = reqId.client_id();
        uint32_t clientSeq = reqId.client_seq();
        bool found = false;

        // If not found, search own log
        if (!found) {
            for (uint32_t seq = log_->getCommittedCheckpoint().seq + 1; seq < log_->getNextSeq(); seq++) {
                const ::LogEntry &entry = log_->getEntry(seq);
                if (entry.client_id == clientId && entry.client_seq == clientSeq) {
                    auto *reqData = reply.add_requests();
                    reqData->set_client_id(clientId);
                    reqData->set_client_seq(clientSeq);
                    reqData->set_request(entry.request);
                    found = true;

                    VLOG(2) << "Providing missing request from own log at seq=" << seq << " c_id=" << clientId
                            << " c_seq=" << clientSeq << " to replica " << fetchRequest.replica_id();
                    break;
                }
            }
        }

        if (!found) {
            VLOG(2) << "Don't have requested request c_id=" << clientId << " c_seq=" << clientSeq;
        }
    }

    if (reply.requests_size() > 0) {
        LOG(INFO) << "Sending MISSING_REQUEST_REPLY to replica " << fetchRequest.replica_id() << " with "
                  << reply.requests_size() << " requests";
        sendMsgToDst(reply, MessageType::MISSING_REQUEST_REPLY, replicaAddrs_[fetchRequest.replica_id()]);
    } else {
        LOG(WARNING) << "No missing requests found to send to replica " << fetchRequest.replica_id();
    }
}

void Replica::processMissingRequestReply(const dombft::proto::MissingRequestReply &fetchReply)
{
    LOG(INFO) << "Processing MISSING_REQUEST_REPLY from replica " << fetchReply.replica_id() << " with "
              << fetchReply.requests_size() << " requests";

    if (fetchReply.round() < round_) {
        VLOG(1) << "Missing request reply round outdated, skipping";
        return;
    }

    // Add the received requests to our repairQueuedReqs_
    for (const auto &reqData : fetchReply.requests()) {
        RequestId key = {reqData.client_id(), reqData.client_seq()};

        // Check if this is one of our pending missing requests
        auto it = std::find(pendingMissingRequests_.begin(), pendingMissingRequests_.end(), key);
        if (it != pendingMissingRequests_.end()) {
            // Add to repairQueuedReqs_
            dombft::proto::ClientRequest clientReq;
            clientReq.set_client_id(reqData.client_id());
            clientReq.set_client_seq(reqData.client_seq());
            clientReq.set_req_data(reqData.request());

            repairQueuedReqs_[{reqData.client_id(), reqData.client_seq()}] = clientReq;

            // Remove from pending list
            pendingMissingRequests_.erase(it);

            LOG(INFO) << "Received missing request c_id=" << reqData.client_id() << " c_seq=" << reqData.client_seq();
        }
    }

    // If all pending requests have been received, try to finish repair again
    if (pendingMissingRequests_.empty() && missingRequestFetchSent_) {
        LOG(INFO) << "All missing requests received, retrying repair";
        missingRequestFetchSent_ = false;
        tryFinishRepair();
    }
}

void Replica::sendMissingRequestFetch(const std::vector<std::pair<uint32_t, uint32_t>> &missingRequests)
{
    if (missingRequests.empty()) {
        return;
    }

    LOG(INFO) << "Sending MISSING_REQUEST_FETCH for " << missingRequests.size() << " requests";

    dombft::proto::MissingRequestFetch fetchRequest;
    fetchRequest.set_round(round_);
    fetchRequest.set_replica_id(replicaId_);

    for (const auto &[clientId, clientSeq] : missingRequests) {
        auto *reqId = fetchRequest.add_request_ids();
        reqId->set_client_id(clientId);
        reqId->set_client_seq(clientSeq);

        VLOG(2) << "Requesting missing request c_id=" << clientId << " c_seq=" << clientSeq;
    }

    // Store the pending requests
    pendingMissingRequests_ = missingRequests;
    missingRequestFetchSent_ = true;

    // Broadcast to all replicas
    broadcastToReplicas(fetchRequest, MessageType::MISSING_REQUEST_FETCH);
}

void Replica::processRepairTimeout(const dombft::proto::RepairTimeout &msg, std::span<byte> sig)
{
    // Note assume msg is verfied here
    if (repair_) {
        VLOG(4) << "Received repair replica timeout during a repair from replica " << msg.replica_id();
        return;
    }

    VLOG(4) << "Received repair replica timeout from " << msg.replica_id() << " for round " << msg.round() << " seq "
            << msg.seq() << " view " << msg.view();

    uint32_t repId = msg.replica_id();
    uint32_t seq = msg.seq();

    if (msg.round() < round_) {
        VLOG(4) << "Received repair timeout for previous round " << msg.round() << " < " << round_;
        return;
    }

    if (msg.view() < pbftView_) {
        VLOG(4) << "Received repair timeout for previous pbft view " << msg.view() << " < " << pbftView_;
        return;
    }

    if (!checkpointCollectors_.hasCollector(round_, seq)) {
        VLOG(4) << "No checkpoint collector for round " << round_ << " seq " << seq << ", creating one now";
        return;
    }

    auto &cc = checkpointCollectors_.at(round_, seq);
    if (cc.addAndCheckTimeout(msg, sig)) {
        dombft::proto::RepairTimeoutProof proof;

        cc.getRepairTimeoutProof(proof);
        LOG(INFO) << "Gathered timeout proof for round " << round_ << ", starting repair and broadcasting!";

        // Reset timeouts
        proof.set_replica_id(replicaId_);
        proof.set_view(pbftView_);
        proof.set_round(round_);

        broadcastToReplicas(proof, REPAIR_TIMEOUT_PROOF);
        startRepair();
    }
}

void Replica::processRepairReplyProof(const dombft::proto::RepairReplyProof &msg)
{
    updateReplicaView(msg.replica_id(), msg.view(), msg.round());

    // Proof is verified by verify thread

    if (msg.round() > round_) {
        LOG(ERROR) << "Received repair trigger proof for round " << msg.round() << " > " << round_;
        // TODO, we need to handle this after repair finishes?
        pendingRepair_ = true;
        return;
    }

    // Ignore repeated repair triggers
    if (repair_) {
        VLOG(6) << "Received repair trigger during a repair";
        return;
    }

    if (msg.round() < round_) {
        VLOG(6) << "Received repair trigger proof for previous round " << msg.round() << " < " << round_;
        return;
    }

    if (msg.view() < pbftView_) {
        VLOG(4) << "Received repair timeout for previous pbft view " << msg.view() << " < " << pbftView_;
        return;
    }

    // Print out proof
    std::ostringstream oss;
    oss << "round=" << round_ << "\n";
    for (int i = 0; i < msg.replies().size(); i++) {
        const auto &reply = msg.replies(i);
        oss << reply.replica_id() << " " << digest_to_hex(reply.digest()) << " " << reply.seq() << " " << reply.round()
            << "\n";
    }

    LOG(INFO) << "Repair proof from " << msg.replica_id() << ":\n" << oss.str();

    // TODO skip sending to ourself, we implictly don't repeat processing this message because we ignore proofs
    // if we already are in fallback.

    dombft::proto::RepairReplyProof proofToBroadcast = msg;
    proofToBroadcast.set_replica_id(replicaId_);
    proofToBroadcast.set_view(pbftView_);

    broadcastToReplicas(proofToBroadcast, REPAIR_REPLY_PROOF);
    startRepair();
}

void Replica::processRepairTimeoutProof(const dombft::proto::RepairTimeoutProof &msg)
{
    updateReplicaView(msg.replica_id(), msg.view(), msg.round());

    if (msg.round() > round_) {
        LOG(ERROR) << "WARNING Received repair trigger proof for future round " << msg.round() << " > " << round_;
        pendingRepair_ = true;
        return;
    }

    // Ignore repeated repair triggers
    if (repair_) {
        VLOG(5) << "Received timeout proof after I already started repair for round " << round_;
        return;
    }

    // Proof is verified by verify thread
    if (msg.round() < round_) {
        LOG(INFO) << "Received repair timeout proof for previous round " << msg.round() << " < " << round_;
        return;
    }

    LOG(INFO) << "Received repair timeout proof, starting repair!";

    // TODO skip sending to ourself, we implictly don't repeat processing this message because we ignore proofs
    // if we already are in fallback.
    broadcastToReplicas(msg, REPAIR_TIMEOUT_PROOF);
    startRepair();
}

void Replica::processRepairStart(const RepairStart &msg, std::span<byte> sig)
{
    if ((msg.pbft_view() % replicaAddrs_.size()) != replicaId_) {
        LOG(INFO) << "Received REPAIR_START for round " << msg.round() << " pbft_view " << msg.pbft_view()
                  << " where I am not proposer";
        return;
    }

    uint32_t repId = msg.replica_id();
    uint32_t repRound = msg.round();
    if (msg.round() < round_) {
        LOG(INFO) << "Received REPAIR_START for round " << msg.round() << " from replica " << repId << " while own is "
                  << round_;
        return;
    }

    // A corner case where (older round + pbft_view) targets the same primary and overwrite the newer ones
    if (repairStartMsgs_.count(repId) && repairStartMsgs_[repId].round() >= repRound) {
        LOG(INFO) << "Received REPAIR_START for round " << repRound << " pbft_view " << msg.pbft_view()
                  << " from replica " << repId << " which is outdated";
        return;
    }
    repairStartMsgs_[repId] = msg;
    repairHistorySigs_[repId] = std::string(sig.begin(), sig.end());

    LOG(INFO) << "Received repairStart message from replica " << repId;

    if (!isPrimary()) {
        return;
    }
    // First check if we have 2f + 1 repair start messages for the same round
    auto numStartMsgs = std::count_if(repairStartMsgs_.begin(), repairStartMsgs_.end(), [&](auto &startMsg) {
        return startMsg.second.round() == repRound;
    });

    if (numStartMsgs == quorumSize_) {

        VLOG(1) << "PERF event=repair_proposal replica_id=" << replicaId_ << " round=" << round_
                << " pbft_view=" << pbftView_;

        doPrePreparePhase(repRound);
    }
}

void Replica::checkTimeouts()
{
    uint64_t now = GetMicrosecondTimestamp();

    // TODO direct access here is wrong
    for (auto &coll : checkpointCollectors_.collectors_) {

        uint64_t replicaCheckpointTimeout = ConfigManager::getInstance().getConfig().replicaCheckpointTimeout;
        if (coll.second.checkSelfTimeout(now, replicaCheckpointTimeout)) {
            dombft::proto::RepairTimeout timeout;
            timeout.set_replica_id(replicaId_);
            timeout.set_round(round_);
            timeout.set_view(pbftView_);
            timeout.set_seq(coll.first.second);

            broadcastToReplicas(timeout, MessageType::REPAIR_TIMEOUT);
        }
    }

    uint64_t checkpointTimeout = ConfigManager::getInstance().getConfig().replicaCheckpointTimeout;
    if (checkpointTimeoutStart_ != 0 && now - checkpointTimeoutStart_ > checkpointTimeout &&
        !checkpointCollectors_.hasCollector(round_, log_->getNextSeq() - 1)) {

        LOG(INFO) << "Starting checkpoint for round=" << round_ << " seq=" << log_->getNextSeq() - 1
                  << " due to timeout!";

        // These are triggering during repair, repair should cancel this or cause it to be ignored
        VLOG(1) << "PERF event=checkpoint_timeout_self" << " seq=" << log_->getNextSeq() - 1 << " round=" << round_
                << " replica_id=" << replicaId_;

        startCheckpoint(false);
        checkpointTimeoutStart_ = 0;
    }

    if (repairViewStart_ != 0 && now - repairViewStart_ > repairViewTimeout_ * (1 << numConsecutiveViewChanges_)) {
        repairViewStart_ = now;
        numConsecutiveViewChanges_ += 1;

        LOG(WARNING) << "Repair for round=" << round_ << " pbft_view=" << pbftView_ << " failed (timed out)!"
                     << " numConsecutiveViewChanges=" << numConsecutiveViewChanges_;
        this->startViewChange(pbftView_ + 1);
    };
}

// ============== Sending Helpers ==============

void Replica::sendSnapshotRequest(uint32_t replicaId, uint32_t targetSeq)
{
    LOG(INFO) << "Sending snapshot request to replica " << replicaId;

    SnapshotRequest snapshotRequest;
    snapshotRequest.set_replica_id(replicaId_);
    snapshotRequest.set_seq(targetSeq);
    snapshotRequest.set_round(round_);
    snapshotRequest.set_last_checkpoint_seq(log_->getCommittedCheckpoint().seq);
    sendMsgToDst(snapshotRequest, MessageType::SNAPSHOT_REQUEST, replicaAddrs_[replicaId]);
}

template <typename T> void Replica::sendMsgToDst(const T &msg, MessageType type, const Address &dst)
{
    if (crashed_) {
        return;
    }

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

template <typename T> void Replica::broadcastToReplicas(const T &msg, MessageType type)
{
    // Simulate crashing by just not sending anything
    if (crashed_) {
        return;
    }

    sendThreadpool_.enqueueTask([=, this](byte *buffer) {
        MessageHeader *hdr = endpoint_->PrepareProtoMsg(msg, type, buffer);
        sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);

        assert(sigProvider_.verify(hdr, {NodeType::REPLICA, replicaId_}));

        for (const Address &addr : replicaAddrs_) {
            endpoint_->SendPreparedMsgTo(addr, hdr);
        }
    });
}

// ============== Verify Helpers ==============

bool Replica::verifyCert(const Cert &cert)
{

    // TODO fix, if superQuorum size is smaller, than we don't need to do any checks
    if (cert.replies().size() < std::min(superQuorumSize_, quorumSize_)) {
        LOG(INFO) << "Received cert of size " << cert.replies().size() << ", which is smaller than 2f + 1, f=" << f_
                  << " quorumSize_=" << quorumSize_;
        return false;
    }

    if (cert.replies().size() != cert.signatures().size()) {
        LOG(INFO) << "Cert replies size " << cert.replies().size() << " is not equal to " << "cert signatures size"
                  << cert.signatures().size();
        return false;
    }

    // check if the replies are matching and no duplicate replies from same replica
    std::map<ReplyKey, std::unordered_set<uint32_t>> matchingReplies;
    // Verify each signature in the cert
    for (int i = 0; i < cert.replies().size(); i++) {
        const Reply &reply = cert.replies()[i];
        const std::string &sig = cert.signatures()[i];
        uint32_t replicaId = reply.replica_id();

        ReplyKey key = {reply.seq(),        reply.round(),  reply.client_id(),
                        reply.client_seq(), reply.digest(), reply.result()};
        matchingReplies[key].insert(replicaId);

        std::string serializedReply = reply.SerializeAsString();
        if (!sigProvider_.verify(
                (byte *) serializedReply.c_str(), serializedReply.size(), (byte *) sig.c_str(), sig.size(),
                {NodeType::REPLICA, reply.replica_id()}
            )) {
            LOG(INFO) << "Cert failed to verify!";
            return false;
        }
    }
    if (matchingReplies.size() > 1) {
        LOG(WARNING) << "Cert has non-matching replies!";
        return false;
    }
    if (matchingReplies.begin()->second.size() < cert.replies().size()) {
        LOG(WARNING) << "Cert has replies from the same replica!";
        return false;
    }

    return true;
}

bool Replica::verifyRepairReplyProof(const RepairReplyProof &proof)
{
    if (proof.replies().size() < f_ + 1) {
        // TODO This is trigering even with correct clients.
        VLOG(2) << "Received repair proof of size " << proof.replies().size()
                << ", which is smaller than f + 1, f=" << f_;
        return false;
    }

    if (proof.replies().size() != proof.signatures().size()) {
        LOG(WARNING) << "Proof replies size " << proof.replies().size() << " is not equal to "
                     << "proof signatures size" << proof.signatures().size();
        return false;
    }

    uint32_t round = proof.round();

    // check if the replies are matching and no duplicate replies from same replica
    std::map<ReplyKey, std::unordered_set<uint32_t>> matchingReplies;
    // Verify each signature in the proof
    for (int i = 0; i < proof.replies_size(); i++) {
        const Reply &reply = proof.replies()[i];
        const std::string &sig = proof.signatures()[i];
        uint32_t replicaId = reply.replica_id();

        if (round != reply.round()) {
            LOG(INFO) << "Proof has replies from different rounds!";
            return false;
        }

        ReplyKey key = {reply.seq(),        reply.round(),  reply.client_id(),
                        reply.client_seq(), reply.digest(), reply.result()};

        matchingReplies[key].insert(replicaId);
        std::string serializedReply = proof.replies(i).SerializeAsString();
        if (!sigProvider_.verify(
                (byte *) serializedReply.c_str(), serializedReply.size(), (byte *) sig.c_str(), sig.size(),
                {NodeType::REPLICA, reply.replica_id()}
            )) {
            LOG(INFO) << "Proof failed to verify!";
            return false;
        }
    }

    if (matchingReplies.size() == 1) {
        LOG(WARNING) << "Proof does not have non-matching replies!";
        return false;
    }

    // TODO verify the math

    uint32_t sum = 0;
    for (auto &[_, s] : matchingReplies) {
        sum += s.size();
    }

    if (sum < proof.replies().size()) {
        LOG(WARNING) << "Proof has replies from the same replica!";
        return false;
    }
    return true;
}

bool Replica::verifyRepairTimeoutProof(const RepairTimeoutProof &proof)
{
    if (proof.timeouts().size() < f_ + 1) {
        LOG(INFO) << "Received repair timeout proof of size " << proof.timeouts().size()
                  << ", which is smaller than f + 1, f=" << f_;
        return false;
    }

    if (proof.timeouts().size() != proof.signatures().size()) {
        LOG(WARNING) << "Proof replies size " << proof.timeouts().size() << " is not equal to "
                     << "cert signatures size" << proof.signatures().size();
        return false;
    }

    std::set<int> replicaIds;

    for (int i = 0; i < proof.timeouts_size(); i++) {
        const RepairTimeout &timeout = proof.timeouts()[i];
        const std::string &sig = proof.signatures()[i];

        if (replicaIds.contains(timeout.replica_id())) {
            LOG(INFO) << "Proof has replies from the same replica!";
            return false;
        }
        replicaIds.insert(timeout.replica_id());

        if (proof.round() != timeout.round()) {
            LOG(INFO) << "Proof has replies from different rounds!";
            return false;
        }

        std::string serializedTimeout = timeout.SerializeAsString();
        if (!sigProvider_.verify(
                (byte *) serializedTimeout.c_str(), serializedTimeout.size(), (byte *) sig.c_str(), sig.size(),
                {NodeType::REPLICA, timeout.replica_id()}
            )) {
            LOG(INFO) << "Proof failed to verify!";
            return false;
        }
    }

    return true;
}

bool Replica::verifyCheckpoint(const LogCheckpoint &checkpoint)
{
    // We don't actually need to verify proofs of the commits in the checkpoint, since
    //  (a) in the fast path the log up to the checkpoint is committed since it is the fast path
    //  (b) in the repair path all n - f - f = (f + 2e + 1) correct replicas will have the previous repiar
    //  checkpoint
    // TODO we should remove these entirely. However, if we have the normal path this is not the case.

    return true;

    if (checkpoint.commits().size() != checkpoint.commit_sigs().size()) {
        return false;
    }

    if (!(checkpoint.commits().size() != superQuorumSize_ || checkpoint.repair_commits().size() != quorumSize_)) {

        LOG(INFO) << "Checkpoint commits not the right size!!";
        return false;
    }

    // TODO this could be optimized by checking equality to our own or other existing checkpoints
    std::set<uint32_t> replicaIds;
    for (int i = 0; i < checkpoint.commits().size(); i++) {
        const Commit &commit = checkpoint.commits()[i];
        const std::string &sig = checkpoint.commit_sigs()[i];
        const std::string serializedCommit = commit.SerializeAsString();

        if (replicaIds.contains(commit.replica_id())) {
            LOG(INFO) << "Checkpoint commits contains multiple of the same replica!";
            return false;
        }
        replicaIds.insert(commit.replica_id());

        if (commit.log_digest() != checkpoint.log_digest()) {
            LOG(INFO) << "Checkpoint commit digest does not match " << digest_to_hex(commit.log_digest())
                      << " != " << digest_to_hex(checkpoint.log_digest());
            return false;
        }

        if (!sigProvider_.verify(
                (byte *) serializedCommit.c_str(), serializedCommit.size(), (byte *) sig.c_str(), sig.size(),
                {NodeType::REPLICA, commit.replica_id()}
            )) {
            LOG(INFO) << "Failed to verify replica signature in repair log checkpoint!";
            return false;
        }
    }

    // TODO this could be optimized by checking equality to our own or other existing checkpoints
    replicaIds.clear();
    for (int i = 0; i < checkpoint.repair_commits().size(); i++) {
        const PBFTCommit &commit = checkpoint.repair_commits()[i];
        const std::string &sig = checkpoint.repair_commit_sigs()[i];
        const std::string serializedCommit = commit.SerializeAsString();

        if (replicaIds.contains(commit.replica_id())) {
            LOG(INFO) << "Checkpoint repair commits contains multiple of the same replica!";
            return false;
        }
        replicaIds.insert(commit.replica_id());

        if (commit.log_digest() != checkpoint.log_digest()) {
            LOG(INFO) << "Checkpoint commit digest does not match Repair commits " << digest_to_hex(commit.log_digest())
                      << " != " << digest_to_hex(checkpoint.log_digest());
            return false;
        }

        if (!sigProvider_.verify(
                (byte *) serializedCommit.c_str(), serializedCommit.size(), (byte *) sig.c_str(), sig.size(),
                {NodeType::REPLICA, commit.replica_id()}
            )) {
            LOG(INFO) << "Failed to verify replica signature in repair log checkpoint!";
            return false;
        }
    }

    return true;
}

bool Replica::verifyRepairStart(const RepairStart &startMsg)
{
    if (startMsg.log().has_cert() && !verifyCert(startMsg.log().cert())) {
        // assert normal path is enabled
        assert(ConfigManager::getInstance().getConfig().clientNormalPathEnabled);

        return false;
    }

    if (!verifyCheckpoint(startMsg.log().checkpoint())) {
        LOG(INFO) << "Failed to verify checkpoint in log from " << startMsg.replica_id();

        return false;
    }

    for (auto &entry : startMsg.log().entries()) {
        // TODO verify log entries
    }

    // Verify repairPrepareHistory if needed
    if (startMsg.has_prepared_history()) {
        const auto &preparedHistory = startMsg.prepared_history();

        // Check that we have at least quorum size prepare messages
        if (preparedHistory.prepares_size() < quorumSize_) {
            LOG(INFO) << "Prepare history from " << startMsg.replica_id()
                      << " has insufficient prepares: " << preparedHistory.prepares_size()
                      << " < quorumSize_=" << quorumSize_;
            return false;
        }

        // Check that number of prepares matches number of signatures
        if (preparedHistory.prepares_size() != preparedHistory.prepare_sigs_size()) {
            LOG(INFO) << "Prepare history from " << startMsg.replica_id()
                      << " has mismatched prepare/signature counts: " << preparedHistory.prepares_size()
                      << " prepares, " << preparedHistory.prepare_sigs_size() << " signatures";
            return false;
        }

        // Verify each prepare message signature
        for (int i = 0; i < preparedHistory.prepares_size(); i++) {
            const auto &prepare = preparedHistory.prepares(i);
            const auto &sig = preparedHistory.prepare_sigs(i);

            std::string serializedPrepare = prepare.SerializeAsString();
            if (!sigProvider_.verify(
                    (byte *) serializedPrepare.c_str(), serializedPrepare.size(), (byte *) sig.c_str(), sig.size(),
                    {NodeType::REPLICA, prepare.replica_id()}
                )) {
                LOG(INFO) << "Failed to verify prepare signature from replica " << prepare.replica_id()
                          << " in prepared history from " << startMsg.replica_id();
                return false;
            }
        }
    }

    return true;
}

bool Replica::verifyRepairProposal(const RepairProposal &proposal)
{
    std::vector<std::tuple<byte *, uint32_t>> logSigs;
    for (auto &sig : proposal.signatures()) {
        logSigs.emplace_back((byte *) sig.data(), sig.length());
    }
    uint32_t ind = 0;

    for (auto &startMsg : proposal.start_msgs()) {
        std::string msgStr = startMsg.SerializeAsString();
        byte *msgBuffer = (byte *) msgStr.data();
        byte *msgSig = std::get<0>(logSigs[ind]);
        uint32_t logSigLen = std::get<1>(logSigs[ind]);
        ind++;
        if (!sigProvider_.verify(
                msgBuffer, msgStr.length(), msgSig, logSigLen, {NodeType::REPLICA, startMsg.replica_id()}
            )) {
            LOG(INFO) << "Failed to verify replica signature from " << startMsg.replica_id() << " in repair proposal!";
            return false;
        }

        if (!verifyRepairStart(startMsg)) {
            LOG(INFO) << "Failed to verify repair start message from " << startMsg.replica_id();
            return false;
        }
    }

    return true;
}

// ============== Repair ==============

void Replica::startRepair()
{
    assert(!repair_);
    repair_ = true;
    LOG(INFO) << "Starting repair on round " << round_;

    VLOG(1) << "PERF event=repair_start replica_id=" << replicaId_ << " seq=" << log_->getNextSeq()
            << " round=" << round_ << " pbft_view=" << pbftView_;

    // Extract log into start repair message
    repairStart_ = RepairStart();
    repairStart_->set_round(round_);
    repairStart_->set_replica_id(replicaId_);
    repairStart_->set_pbft_view(pbftView_);

    // Include full requests based on config
    bool includeFullRequests = dombft::ConfigManager::getInstance().getConfig().replicaIncludeFullRequests;
    log_->toProto(*repairStart_, includeFullRequests);

    uint32_t primaryId = getPrimary();
    VLOG(2) << "Sending REPAIR_START to PBFT primary replica " << primaryId;
    sendMsgToDst(*repairStart_, REPAIR_START, replicaAddrs_[primaryId]);
    VLOG(2) << "PERF_DUMP start repair round=" << round_ << " " << *log_;
}

void Replica::sendRepairSummaryToClients()
{
    std::map<uint32_t, RepairSummary> messages;

    uint32_t seq = log_->getCommittedCheckpoint().seq + 1;
    for (; seq < log_->getNextSeq(); seq++) {
        const ::LogEntry &entry = log_->getEntry(seq);   // TODO better namespace

        if (!messages.contains(entry.client_id)) {
            RepairSummary &summary = messages[entry.client_id];
            summary.set_round(round_);
            summary.set_replica_id(replicaId_);
            summary.set_pbft_view(pbftView_);

            log_->getCommittedCheckpoint().clientRecord_.toProtoSingleClient(
                entry.client_id, *summary.mutable_committed_seqs()
            );
        }

        RepairSummary &summary = messages[entry.client_id];

        CommittedReply reply;

        reply.set_replica_id(replicaId_);
        reply.set_client_id(entry.client_id);
        reply.set_client_seq(entry.client_seq);
        reply.set_seq(entry.seq);
        reply.set_result(entry.result);
        reply.set_is_repair(true);

        (*summary.add_replies()) = reply;
    }

    for (auto &[clientId, summary] : messages) {
        sendMsgToDst(summary, MessageType::REPAIR_SUMMARY, clientAddrs_[clientId]);
    }
}

void Replica::finishRepair(const std::vector<::ClientRequest> &abortedReqs)
{
    VLOG(1) << "PERF event=repair_end replica_id=" << replicaId_ << " seq=" << log_->getNextSeq() << " round=" << round_
            << " pbft_view=" << pbftView_;

    VLOG(2) << "PERF_DUMP finish repair round=" << round_ << " " << *log_;
    // LOG(INFO) << "Current client record" << log_->getClientRecord();
    // LOG(INFO) << "Checkpoint client record" << log_->getCommittedCheckpoint().clientRecord_;

    round_++;
    VLOG(2) << "Round updated to " << round_ << " and pbft_view to " << pbftView_;

    if (numConsecutiveViewChanges_ > 0) {
        LOG(INFO) << "Repair for round=" << round_ - 1 << " pbft_view=" << pbftView_ << " finished after "
                  << numConsecutiveViewChanges_ << " view changes.";
        viewChangeCounter_ += 1;
        numConsecutiveViewChanges_ = 0;
    }

    // Send repair summary to clients to allow commits in the slow path..
    // NOTE: there was a bug where this was after the checkpointing and so was empty
    sendRepairSummaryToClients();

    uint32_t startSeq = getRepairLogSuffix().checkpoint->seq();

    // TODO: since the repair is PBFT, we can simply set the checkpoint here already using the PBFT messages as
    // proofs For the sake of implementation simplicity, we just trigger the usual checkpointing process However,
    // client requests are still safe, as even if the next repair is triggered before this checkpoint finishes, the
    // client requests will have f + 1 replicas that executed it in their logs

    uint32_t seq = log_->getNextSeq() - 1;

    if (seq <= log_->getCommittedCheckpoint().seq) {
        LOG(ERROR) << "Repair finished, but no new checkpoint to commit?";
    } else {

        // Repair round crosses a snapshot interavl, we should also kick off one
        // The reason we need to do this is because if repair is continuously triggered,
        // replicas may never be able to gather a cert before the next repair is triggered

        // TODO
        if (seq / snapshotInterval_ > startSeq / snapshotInterval_) {
            // TODO this is also a bit hacky...
            // Create a checkpoint with a new stable app digest by doing a commit round!
            VLOG(3) << "Starting new commit round for seq=" << seq
                    << " to create a new application snapshot directly from commit!";

            VLOG(2) << "PERF event=checkpoint_start seq=" << seq << " createSnapshot=1"
                    << " round=" << round_ << " log_digest=" << digest_to_hex(log_->getDigest());

            Commit commit;
            commit.set_replica_id(replicaId_);
            commit.set_round(round_);

            commit.set_seq(seq);
            commit.set_log_digest(log_->getDigest(seq));

            log_->getClientRecord().toProto(*commit.mutable_client_record());

            if (!checkpointCollectors_.hasCollector(round_, seq)) {
                checkpointCollectors_.initCollector(round_, seq, true);
            }

            uint32_t round = round_;
            app_->takeSnapshot([&, round, commit](const AppSnapshot &snapshot) {
                Commit c = commit;
                c.set_app_digest(snapshot.digest);

                VLOG(3) << "Snapshot taken for round=" << round << " seq=" << snapshot.seq
                        << " digest=" << digest_to_hex(snapshot.digest);

                // Queue to be processed by main thread
                // TODO this needs to happen first, otherwise the snapshot will be missed
                snapshotQueue_.enqueue({round, snapshot});

                // Commit own
                broadcastToReplicas(c, MessageType::COMMIT);
            });

        } else {
            // Otherwise save a new checkpoint right away
            ::LogCheckpoint newCheckpoint;

            newCheckpoint.seq = seq;
            newCheckpoint.logDigest = log_->getDigest(seq);

            for (auto &[rId, c] : repairPBFTCommits_) {
                newCheckpoint.repairCommits[rId] = c;
                newCheckpoint.repairCommitSigs[rId] = repairCommitSigs_[rId];
            }

            newCheckpoint.clientRecord_ = log_->getClientRecord();

            VLOG(1) << "PERF event=checkpoint_repair replica_id=" << replicaId_ << " seq=" << seq << " round=" << round_
                    << " pbft_view=" << pbftView_ << " log_digest=" << digest_to_hex(newCheckpoint.logDigest);

            if (newCheckpoint.logDigest == newCheckpoint.repairCommits.begin()->second.log_digest()) {
                log_->setCheckpoint(newCheckpoint);

            } else {
                // TODO this should be an assert, but we will fix this later.
                LOG(ERROR) << "Repair commit digests "
                           << digest_to_hex(newCheckpoint.repairCommits.begin()->second.log_digest())
                           << " does not match my log digest " << digest_to_hex(newCheckpoint.logDigest)
                           << " skipping...";
            }
        }
    }

    repair_ = false;
    repairProposal_.reset();
    repairPrepares_.clear();
    repairPBFTCommits_.clear();
    repairProposalLogSuffix_.reset();
    missingRequestFetchSent_ = false;

    // Reapply any requests that were aborted in previous round
    // NOTE, this is actually allow a single byzantine client to prevent a replica from ever entering fast path...

    for (auto &req : abortedReqs) {
        ClientRequest clientReq;
        clientReq.set_client_id(req.clientId);
        clientReq.set_client_seq(req.clientSeq);
        clientReq.set_req_data(req.requestData);

        VLOG(5) << "Adding aborted request client_id=" << req.clientId << " client_seq=" << req.clientSeq;

        repairQueuedReqs_.insert({{req.deadline, req.clientId}, clientReq});
    }

    curRoundStartSeq_ = log_->getNextSeq();

    // Retry any requests

    for (auto &[_, req] : repairQueuedReqs_) {
        VLOG(5) << "Processing queued request client_id=" << req.client_id() << " client_seq=" << req.client_seq();
        processClientRequest(req, true);
    }
    repairQueuedReqs_.clear();

    // Start timer for next checkpoint
    checkpointTimeoutStart_ = GetMicrosecondTimestamp();

    VLOG(2) << "PERF_DUMP post repair round=" << round_ - 1 << " " << *log_;

    // TODO hack if we received proof for next repair round in this round...
    if (pendingRepair_) {
        pendingRepair_ = false;
        startRepair();
    }
}

void Replica::tryFinishRepair()
{
    LogSuffix &logSuffix = getRepairLogSuffix();

    assert(repairProposal_.has_value());
    if (repairProposal_.value().round() == round_ - 1) {
        // This happens if the repair round is already committed on the current replica, but other replicas
        // initiated a view change.
        LOG(INFO) << "Repair on round " << round_ - 1 << " already committed on current replica, skipping";

        std::vector<::ClientRequest> abortedRequests = getAbortedEntries(logSuffix, log_, curRoundStartSeq_);
        finishRepair(abortedRequests);
        return;
    }

    RepairProposal &proposal = repairProposal_.value();
    round_ = proposal.round();
    // Reset timer
    repairViewStart_ = 0;

    // Check if own checkpoint seq is behind suffix checkpoint seq
    const dombft::proto::LogCheckpoint *checkpoint = logSuffix.checkpoint;

    VLOG(1) << "PERF event=repair_apply replica_id=" << replicaId_ << " seq=" << log_->getNextSeq()
            << " round=" << round_ << " pbft_view=" << pbftView_;

    ::LogCheckpoint &myCheckpoint = log_->getCommittedCheckpoint();   // bad namespace
    if (checkpoint->seq() > myCheckpoint.seq) {
        // If our log is consistent, we can just use the checkpoint, otherwise
        // we need to request a snapshot from the replica that has the checkpoint

        if (checkpoint->seq() < log_->getNextSeq() && checkpoint->log_digest() == log_->getDigest(checkpoint->seq())) {
            std::vector<::ClientRequest> abortedRequests = getAbortedEntries(logSuffix, log_, curRoundStartSeq_);
            std::map<RequestId, std::string> availableReqs;
            for (auto [_, req] : repairQueuedReqs_) {
                availableReqs[{req.client_id(), req.client_seq()}] = req.req_data();
            }

            std::vector<std::pair<uint32_t, uint32_t>> missingRequests;
            if (!applySuffix(logSuffix, availableReqs, log_, missingRequests)) {
                LOG(INFO) << "applySuffix failed due to missing requests, requesting from other replicas";
                sendMissingRequestFetch(missingRequests);
                return;
            }
            finishRepair(abortedRequests);
        } else {
            LOG(INFO) << "Repair checkpoint seq=" << checkpoint->seq() << " is inconsistent with my log";

            if (!repairSnapshotRequested_) {
                sendSnapshotRequest(logSuffix.checkpointReplica, checkpoint->seq());
            }
            repairSnapshotRequested_ = true;
        }
    } else {
        std::map<RequestId, std::string> availableReqs;
        for (auto [_, req] : repairQueuedReqs_) {
            availableReqs[{req.client_id(), req.client_seq()}] = req.req_data();
        }

        std::vector<::ClientRequest> abortedRequests = getAbortedEntries(logSuffix, log_, curRoundStartSeq_);
        std::vector<std::pair<uint32_t, uint32_t>> missingRequests;
        if (!applySuffix(logSuffix, availableReqs, log_, missingRequests)) {
            LOG(INFO) << "applySuffix failed due to missing requests, requesting from other replicas";
            sendMissingRequestFetch(missingRequests);
            return;
        }
        finishRepair(abortedRequests);
    }
}

LogSuffix &Replica::getRepairLogSuffix()
{
    // This is just to cache the processing of the repairProposal

    // TODO cache across view changes, if the proposal digest is the same. For now we just recompute every time
    // siince trying to use the wrong proposal can lead to memory issues, since LogSuffix contains pointers into the
    // proposal

    if (!repairProposalLogSuffix_.has_value() || repairProposalLogSuffix_.value().round != round_) {
        repairProposalLogSuffix_ = LogSuffix();
        repairProposalLogSuffix_->replicaId = replicaId_;
        repairProposalLogSuffix_->round = round_;
        getLogSuffixFromProposal(repairProposal_.value(), repairProposalLogSuffix_.value());
    }
    return repairProposalLogSuffix_.value();
}

void Replica::doPrePreparePhase(uint32_t round)
{
    if (!isPrimary()) {
        LOG(ERROR) << "Attempted to doPrePrepare from non-primary replica!";
        return;
    }
    LOG(INFO) << "PrePrepare for pbft_view=" << pbftView_ << " in primary replicaId=" << replicaId_;

    PBFTPrePrepare prePrepare;
    prePrepare.set_primary_id(replicaId_);
    prePrepare.set_round(round);
    prePrepare.set_pbft_view(pbftView_);

    // If some replica has a certificate, use the prepared proposal matching it, instead of using the new
    // history set
    for (auto &startMsg : repairStartMsgs_) {
        if (startMsg.second.round() != round && startMsg.second.pbft_view() != pbftView_)
            continue;

        if (startMsg.second.has_prepared_history()) {
            *(prePrepare.mutable_proposal()) = lastPreparedState_.proposal;
            prePrepare.set_proposal_digest(lastPreparedState_.proposalDigest);
            LOG(INFO) << "Using prepared history due to view change for repair proposal round=" << round
                      << " replicaId=" << replicaId_ << " view=" << pbftView_;
            broadcastToReplicas(prePrepare, PBFT_PREPREPARE);
            return;
        }
    }

    // Piggyback the repair proposal

    LOG(INFO) << "Creating new repair proposal for round=" << round << " replicaId=" << replicaId_
              << " view=" << pbftView_;

    RepairProposal *proposal = prePrepare.mutable_proposal();
    proposal->set_replica_id(replicaId_);
    proposal->set_round(round);
    for (auto &startMsg : repairStartMsgs_) {
        if (startMsg.second.round() != round && startMsg.second.pbft_view() != pbftView_)
            continue;

        *(proposal->add_start_msgs()) = startMsg.second;
        *(proposal->add_signatures()) = repairHistorySigs_[startMsg.first];
    }

    proposalDigest_ = getProposalDigest(prePrepare.proposal());

    // If the view has not committed yet, we add proof of the new view change necessity

    prePrepare.set_proposal_digest(proposalDigest_);
    broadcastToReplicas(prePrepare, PBFT_PREPREPARE);
}

void Replica::doPreparePhase()
{
    LOG(INFO) << "Prepare for round=" << repairProposal_->round() << " replicaId=" << replicaId_;
    PBFTPrepare prepare;
    prepare.set_replica_id(replicaId_);
    prepare.set_round(repairProposal_->round());
    prepare.set_pbft_view(pbftView_);
    prepare.set_proposal_digest(proposalDigest_);

    LogSuffix &logSuffix = getRepairLogSuffix();

    prepare.set_log_digest(logSuffix.logDigest);

    broadcastToReplicas(prepare, PBFT_PREPARE);
}

void Replica::doCommitPhase()
{
    uint32_t proposalInst = repairProposal_.value().round();
    PBFTCommit cmt;
    cmt.set_replica_id(replicaId_);
    cmt.set_round(proposalInst);
    cmt.set_pbft_view(pbftView_);
    cmt.set_proposal_digest(proposalDigest_);

    LogSuffix &logSuffix = getRepairLogSuffix();
    cmt.set_log_digest(logSuffix.logDigest);

    if (viewChangeByCommit()) {
        if (commitLocalInViewChange_)
            sendMsgToDst(cmt, PBFT_COMMIT, replicaAddrs_[replicaId_]);
        holdPrepareOrCommit_ = !holdPrepareOrCommit_;
        return;
    }

    LOG(INFO) << "PBFTCommit for round=" << proposalInst << " replicaId=" << replicaId_
              << " log_digest=" << digest_to_hex(logSuffix.logDigest);

    broadcastToReplicas(cmt, PBFT_COMMIT);
}

void Replica::processPrePrepare(const PBFTPrePrepare &msg)
{
    if (msg.round() < round_) {
        LOG(INFO) << "Received old repair preprepare from round=" << msg.round() << " own round is " << round_;
        return;
    }

    if (msg.round() > round_) {
        LOG(INFO) << "Received future repair preprepare from round=" << msg.round() << " own round is " << round_;
        return;
    }

    if (msg.pbft_view() != pbftView_) {
        LOG(INFO) << "Received preprepare from replicaId=" << msg.primary_id() << " for round=" << msg.round()
                  << " with different pbft_view=" << msg.pbft_view();
        return;
    }
    if (getPrimary() != msg.primary_id()) {
        LOG(INFO) << "Received repair preprepare from non-primary replica " << msg.primary_id()
                  << ". Current selected primary is " << getPrimary();
        return;
    }

    LOG(INFO) << "PrePrepare RECEIVED for round=" << msg.round() << " from replicaId=" << msg.primary_id();
    VLOG(1) << "PERF event=repair_preprepare replica_id=" << replicaId_ << " seq=" << log_->getNextSeq()
            << " round=" << msg.round() << " pbft_view=" << pbftView_
            << " proposal_digest=" << digest_to_hex(msg.proposal_digest());

    // accepts the proposal as long as it's from the primary
    repairProposal_ = msg.proposal();
    repairProposalLogSuffix_.reset();
    proposalDigest_ = msg.proposal_digest();

    if (viewChangeByPrepare()) {
        holdPrepareOrCommit_ = !holdPrepareOrCommit_;
        LOG(INFO) << "Prepare message held to cause timeout in prepare phase for view change";
        viewChangeRound_ += viewChangeFreq_;
        return;
    }
    doPreparePhase();
}

void Replica::processPrepare(const PBFTPrepare &msg, std::span<byte> sig)
{
    uint32_t inRound = msg.round();
    if (msg.pbft_view() != pbftView_) {
        LOG(INFO) << "Received prepare from replicaId=" << msg.replica_id() << " for round=" << inRound
                  << " with different pbft_view=" << msg.pbft_view();
        return;
    }
    if (inRound < round_ && viewPrepared_) {
        LOG(INFO) << "Received old repair prepare from round=" << inRound << " own round is " << round_;
        return;
    }

    if (inRound > round_) {
        LOG(INFO) << "Received future repair prepare from round=" << inRound << " own round is " << round_;
        return;
    }

    if (repairPrepares_.count(msg.replica_id()) && repairPrepares_[msg.replica_id()].round() > inRound &&
        repairPrepares_[msg.replica_id()].pbft_view() == msg.pbft_view()) {
        LOG(INFO) << "Old prepare received from replicaId=" << msg.replica_id() << " for round=" << inRound;
        return;
    }
    repairPrepares_[msg.replica_id()] = msg;
    repairPrepareSigs_[msg.replica_id()] = std::string(sig.begin(), sig.end());
    LOG(INFO) << "Prepare RECEIVED for round=" << inRound << " from replicaId=" << msg.replica_id();
    // skip if already prepared for it
    // note: if viewPrepared_==false, then viewChange_==true
    if (viewPrepared_ && preparedRound_ == inRound) {
        LOG(INFO) << "Already prepared for round=" << inRound << " pbft_view=" << pbftView_;
        return;
    }
    if (!repairProposal_.has_value() || repairProposal_.value().round() < inRound) {
        LOG(INFO) << "PrePrepare not received yet, wait till it arrives to process prepare";
        return;
    }
    // Make sure the Prepare msgs are for the corresponding PrePrepare msg
    auto numMsgs = std::count_if(repairPrepares_.begin(), repairPrepares_.end(), [this](auto &curMsg) {
        return curMsg.second.round() == repairProposal_.value().round() &&
               curMsg.second.proposal_digest() == proposalDigest_ && curMsg.second.pbft_view() == pbftView_;
    });
    if (numMsgs < quorumSize_) {
        LOG(INFO) << "Prepare received from " << numMsgs << " replicas, waiting for 2f + 1 to proceed";
        return;
    }
    // Store PBFT states for potential view change
    preparedRound_ = repairProposal_.value().round();
    viewPrepared_ = true;
    lastPreparedState_.round = preparedRound_;
    lastPreparedState_.pbftView = pbftView_;
    lastPreparedState_.proposal = repairProposal_.value();
    lastPreparedState_.proposalDigest = proposalDigest_;
    lastPreparedState_.prepares.clear();
    for (const auto &[repId, prepare] : repairPrepares_) {
        if (prepare.round() == preparedRound_) {
            lastPreparedState_.prepares[repId] = prepare;
            lastPreparedState_.prepareSigs[repId] = repairPrepareSigs_[repId];
        }
    }
    LOG(INFO) << "Prepare received from 2f + 1 replicas, agreement reached for round=" << preparedRound_
              << " pbft_view=" << pbftView_;

    VLOG(1) << "PERF event=prepared replica_id=" << replicaId_ << " seq=" << log_->getNextSeq()
            << " round=" << preparedRound_ << " pbft_view=" << pbftView_
            << " proposal_digest=" << digest_to_hex(msg.proposal_digest());

    if (viewChangeByCommit()) {
        if (commitLocalInViewChange_) {
            LOG(INFO) << "Commit message only send to itself to commit locally to advance to next round";
            doCommitPhase();
        } else {
            LOG(INFO) << "Commit message held to cause timeout in commit phase for view change";
        }
        viewChangeRound_ += viewChangeFreq_;

        return;
    }
    doCommitPhase();
}

void Replica::processPBFTCommit(const PBFTCommit &msg, std::span<byte> sig)
{
    uint32_t inRound = msg.round();
    if (msg.pbft_view() != pbftView_) {
        LOG(INFO) << "Received commit from replicaId=" << msg.replica_id() << " for round=" << inRound
                  << " with different pbft_view=" << msg.pbft_view();
        return;
    }
    if (inRound < round_) {
        LOG(INFO) << "Received old repair commit from round=" << inRound << " own round is " << round_;
        return;
    }

    if (inRound > round_) {
        LOG(INFO) << "Received future repair commit from round=" << inRound << " own round is " << round_;
        return;
    }

    if (repairPBFTCommits_.count(msg.replica_id()) && repairPBFTCommits_[msg.replica_id()].round() > inRound &&
        repairPBFTCommits_[msg.replica_id()].pbft_view() == msg.pbft_view()) {
        LOG(INFO) << "Old commit received from replicaId=" << msg.replica_id() << " for round=" << inRound;
        return;
    }
    repairPBFTCommits_[msg.replica_id()] = msg;
    repairCommitSigs_[msg.replica_id()] = std::string(sig.begin(), sig.end());

    LOG(INFO) << "PBFTCommit RECEIVED for round=" << inRound << " from replicaId=" << msg.replica_id();

    if (!repairProposal_.has_value() || repairProposal_.value().round() < inRound) {
        LOG(INFO) << "PrePrepare not received yet, wait till it arrives to process commit";
        return;
    }

    if (preparedRound_ == UINT32_MAX || preparedRound_ != inRound || !viewPrepared_) {
        LOG(INFO) << "Not prepared for it, skipping commit!";
        // TODO get the proposal from another replica...
        return;
    }
    auto numMsgs = std::count_if(repairPBFTCommits_.begin(), repairPBFTCommits_.end(), [this](auto &curMsg) {
        return curMsg.second.round() == preparedRound_ && curMsg.second.proposal_digest() == proposalDigest_ &&
               curMsg.second.pbft_view() == pbftView_;
    });
    if (numMsgs < quorumSize_) {
        return;
    }

    VLOG(1) << "PERF event=repair_commit replica_id=" << replicaId_ << " seq=" << log_->getNextSeq()
            << " round=" << preparedRound_ << " pbft_view=" << pbftView_
            << " log_digest=" << digest_to_hex(msg.log_digest())
            << " proposal_digest=" << digest_to_hex(msg.proposal_digest());

    LOG(INFO) << "Commit received from 2f + 1 replicas, Committed!";
    tryFinishRepair();
}

void Replica::updateReplicaView(uint32_t replicaId, uint32_t view, uint32_t round)
{
    if (replicaViews_.contains(replicaId) && replicaViews_[replicaId].first == view &&
        replicaViews_[replicaId].second == round) {
        // No change
        return;
    }

    replicaViews_[replicaId] = {view, round};

    // First check if quorumSize_ replicas have the same view as us; in this case
    // we can start our view change timer

    auto numSameView = std::count_if(replicaViews_.begin(), replicaViews_.end(), [this](auto &entry) {
        return entry.second.first == pbftView_ && entry.second.second == round_;
    });

    if (numSameView >= quorumSize_) {
        LOG(INFO) << "Detected quorum of replicas in view=" << pbftView_ << " round=" << round_
                  << ", starting view change timer";
        repairViewStart_ = GetMicrosecondTimestamp();
    }

    // Next check if n - quorumSize + 1 replicas have a higher view than us; in this case we should
    // start a view change immediately
    // Since quorumSize - f > n - quorumSize + 1 => quorumSize > (n + f + 1) / 2 > 4f + 1 / 2 > 2f + 1
    // If quorumSize_ triggered above, this will bring any straggler replicas up to speed as well

    uint32_t numHigher = 0;
    uint32_t minHigherView = UINT32_MAX;

    for (const auto &v : replicaViews_) {

        VLOG(7) << "Replica view status: replicaId=" << v.first << " view=" << v.second.first
                << " round=" << v.second.second;

        if (v.second.first > pbftView_) {
            numHigher++;
            minHigherView = std::min(minHigherView, v.second.first);
        }
    }
    VLOG(7) << "------------------";
    assert(numHigher == 0 || (minHigherView > pbftView_ && minHigherView != UINT32_MAX));

    uint32_t numReplicas = ConfigManager::getInstance().getNumReplicas();
    if (numHigher >= (numReplicas - quorumSize_ + 1)) {
        LOG(INFO) << "Detected that majority of replicas have higher view, starting view change to view "
                  << minHigherView;
        startViewChange(minHigherView);
    }
}

void Replica::startViewChange(uint32_t newView)
{

    pbftView_ = newView;
    repair_ = true;
    viewPrepared_ = false;
    repairViewStart_ = 0;   // Reset view change timer, wait until other replicas have the same view

    VLOG(1) << "PERF event=viewchange_start replica_id=" << replicaId_ << " seq=" << log_->getNextSeq()
            << " round=" << round_ << " pbft_view=" << pbftView_;
    LOG(INFO) << "Starting ViewChange on round " << round_ << " pbft_view " << pbftView_;

    // Add the latest quorum of prepares and sigs to the current RepairStartMessage
    if (lastPreparedState_.round == round_) {

        repairStart_ = RepairStart();
        repairStart_->set_replica_id(replicaId_);
        repairStart_->set_pbft_view(pbftView_);
        repairStart_->set_round(round_);

        dombft::proto::PreparedHistory *preparedHistory = repairStart_->mutable_prepared_history();
        for (const auto &[repId, prepare] : lastPreparedState_.prepares) {
            *(preparedHistory->add_prepares()) = prepare;
            *(preparedHistory->add_prepare_sigs()) = lastPreparedState_.prepareSigs[repId];
        }

        LOG(INFO) << "Sending REPAIR_START with prepared history to PBFT primary replica " << getPrimary();
        sendMsgToDst(*repairStart_, REPAIR_START, replicaAddrs_[getPrimary()]);

    } else {
        // No prepared history for this round, so send normal repair start message that you sent before

        repairStart_->clear_prepared_history();
        assert(repairStart_.has_value());
        assert(repairStart_->round() == round_);
        repairStart_->set_pbft_view(pbftView_);
        LOG(INFO) << "Sending REPAIR_START without prepared history to PBFT primary replica " << getPrimary();
        sendMsgToDst(*repairStart_, REPAIR_START, replicaAddrs_[getPrimary()]);
    }

    if (isPrimary()) {
        // First check if we have 2f + 1 repair start messages for the same round
        auto numStartMsgs = std::count_if(repairStartMsgs_.begin(), repairStartMsgs_.end(), [&](auto &startMsg) {
            return startMsg.second.round() == round_ && startMsg.second.pbft_view() == pbftView_;
        });

        if (numStartMsgs >= quorumSize_) {
            doPrePreparePhase(round_);
        }
    }

    dombft::proto::ViewUpdate viewUpdateMsg;
    viewUpdateMsg.set_replica_id(replicaId_);
    viewUpdateMsg.set_view(pbftView_);
    viewUpdateMsg.set_round(round_);

    broadcastToReplicas(viewUpdateMsg, VIEW_UPDATE);
}

std::string Replica::getProposalDigest(const RepairProposal &proposal)
{
    // use signatures as digest, since signatures are can function as the the digest of each proposal
    CryptoPP::SHA256 hash;
    byte digestBuf[CryptoPP::SHA256::DIGESTSIZE];
    std::string digestStr;

    for (const auto &sig : proposal.signatures()) {
        digestStr += sig;
    }

    hash.Update(reinterpret_cast<const byte *>(digestStr.data()), digestStr.size());
    hash.Final(digestBuf);

    return std::string(reinterpret_cast<const char *>(digestBuf), CryptoPP::SHA256::DIGESTSIZE);
}

// *** EXTRA LOGIC FOR PRESERIALIZATION EXPERIMENTS, not a part of the actual protocol, and not implemented fully ***

void Replica::processPSClient(const dombft::proto::ClientRequest &clientRequest, std::span<byte> sig)
{
    // send wrapped client request to everyone, enqueue it by itself
    if (preserializationMode_ == "full") {
        // Extract client signature from original message
        std::string clientSig((char *) sig.data(), sig.size());
        PSLeaderForward fwd;
        fwd.set_seq(log_->getNextSeq());
        *fwd.mutable_request() = clientRequest;
        fwd.set_client_signature(clientSig);

        // Forward full request to all other replicas as PS_LEADER_FORWARD
        sendThreadpool_.enqueueTask([=, this](byte *buffer) {
            MessageHeader *fwdHdr = endpoint_->PrepareProtoMsg(fwd, MessageType::PS_LEADER_FORWARD, buffer);
            fwdHdr->sigLen = 0;

            for (size_t i = 0; i < replicaAddrs_.size(); i++) {
                if (i != replicaId_) {
                    endpoint_->SendPreparedMsgTo(replicaAddrs_[i], fwdHdr);
                }
            }
        });
        // Process locally
        processClientRequest(clientRequest);

    } else if (preserializationMode_ == "order") {

        if (replicaId_ == 0) {
            PSLeaderOrder order;
            order.set_client_id(clientRequest.client_id());
            order.set_client_seq(clientRequest.client_seq());
            order.set_seq(log_->getNextSeq());

            sendThreadpool_.enqueueTask([=, this](byte *buffer) {
                MessageHeader *fwdHdr = endpoint_->PrepareProtoMsg(order, MessageType::PS_LEADER_ORDER, buffer);
                fwdHdr->sigLen = 0;

                for (size_t i = 0; i < replicaAddrs_.size(); i++) {
                    if (i != replicaId_) {
                        endpoint_->SendPreparedMsgTo(replicaAddrs_[i], fwdHdr);
                    }
                }
            });
            // Process locally
            processClientRequest(clientRequest);

        } else {
            // Non-primary replicas just buffer the request until ordered
            // TODO emplace
            psOrderRequests_[{clientRequest.client_id(), clientRequest.client_seq()}] = clientRequest;
        }
    }
}

void Replica::processPSLeaderForward(const dombft::proto::PSLeaderForward &psForward)
{
    // If nextSeq is seq, process the request as you would

    VLOG(4) << "processPSLeaderForward: seq=" << psForward.seq() << " nextSeq=" << log_->getNextSeq()
            << " client_id=" << psForward.request().client_id() << " client_seq=" << psForward.request().client_seq();

    if (psForward.seq() == log_->getNextSeq()) {
        processClientRequest(psForward.request());

        // Then check the buffer for any subsequent requests
        uint32_t nextSeq = log_->getNextSeq();
        uint32_t processedFromBuffer = 0;
        while (psForwardBuffer_.contains(nextSeq)) {
            processClientRequest(psForwardBuffer_[nextSeq]);
            psForwardBuffer_.erase(nextSeq);
            nextSeq++;
            processedFromBuffer++;
        }

        VLOG(2) << "processPSLeaderForward: processed 1 request + " << processedFromBuffer
                << " from buffer, buffer size now: " << psForwardBuffer_.size();

    } else {
        // Otherwise buffer it until its turn comes
        psForwardBuffer_.emplace(psForward.seq(), psForward.request());
        VLOG(2) << "processPSLeaderForward: buffered seq=" << psForward.seq()
                << ", buffer size now: " << psForwardBuffer_.size();
        checkPSOrderRequests();
    }
}

void Replica::processPSLeaderOrder(const dombft::proto::PSLeaderOrder &psOrder)
{
    VLOG(4) << "processPSLeaderOrder: seq=" << psOrder.seq() << " client_id=" << psOrder.client_id()
            << " client_seq=" << psOrder.client_seq();

    psOrderSeqs_[psOrder.seq()] = {psOrder.client_id(), psOrder.client_seq()};
    checkPSOrderRequests();
}

void Replica::checkPSOrderRequests()
{
    uint32_t processedCount = 0;
    while (psOrderSeqs_.contains(log_->getNextSeq())) {
        uint32_t nextSeq = log_->getNextSeq();
        auto [clientId, clientSeq] = psOrderSeqs_[nextSeq];
        std::pair<uint32_t, uint32_t> key = {clientId, clientSeq};

        VLOG(4) << "checkPSOrderRequests: checking seq=" << nextSeq << " client_id=" << clientId
                << " client_seq=" << clientSeq;

        if (psOrderRequests_.contains(key)) {
            ClientRequest req = psOrderRequests_[key];
            psOrderRequests_.erase(key);
            psOrderSeqs_.erase(nextSeq);
            processClientRequest(req);
            processedCount++;
        } else {
            // Waiting for client request
            VLOG(2) << "checkPSOrderRequests: waiting for client request, processed " << processedCount
                    << " requests, psOrderSeqs size: " << psOrderSeqs_.size()
                    << ", psOrderRequests size: " << psOrderRequests_.size();
            return;
        }
    }

    if (processedCount > 0) {
        VLOG(2) << "checkPSOrderRequests: processed " << processedCount
                << " requests, psOrderSeqs size: " << psOrderSeqs_.size()
                << ", psOrderRequests size: " << psOrderRequests_.size();
    }
}

}   // namespace dombft
